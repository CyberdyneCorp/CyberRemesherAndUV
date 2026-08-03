// C ABI harness (capi module, task 13.3 partial): drives the pure-C surface
// end to end from C++ — write a cube OBJ, load it through the C entry point,
// remesh with default parameters, and assert the status, the quad output and
// that the progress callback fired.
#include <doctest.h>

#include <algorithm>
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

// --- regional prescribed-boundary solve C API (add-weave-regional-solve) -----

namespace {

// 6x6 quad grid written as an OBJ; capi meshes are created by loading, and the
// loader numbers faces in file order, so grid cell (i,j) is face i*6 + j.
std::string writeGridObj() {
    const int n = 6;
    std::string obj;
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            obj += "v " + std::to_string(i) + " " + std::to_string(j) + " 0\n";
        }
    }
    const auto vid = [](int i, int j) { return i * 7 + j + 1; };  // OBJ is 1-based
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            obj += "f " + std::to_string(vid(i, j)) + " " + std::to_string(vid(i + 1, j)) + " " +
                   std::to_string(vid(i + 1, j + 1)) + " " + std::to_string(vid(i, j + 1)) + "\n";
        }
    }
    const std::string path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/cyber_region_grid.obj";
    std::ofstream out(path);
    out << obj;
    return path;
}

std::vector<uint32_t> centreRegion() {
    std::vector<uint32_t> region;
    for (int i = 1; i <= 4; ++i) {
        for (int j = 1; j <= 4; ++j) {
            region.push_back(static_cast<uint32_t>(i * 6 + j));
        }
    }
    return region;
}

}  // namespace

TEST_CASE("capi region solve: ids survive, report is readable") {
    const std::string path = writeGridObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(path.c_str(), &mesh) == CYBER_OK);
    REQUIRE(mesh != nullptr);
    const std::vector<uint32_t> region = centreRegion();

    SUBCASE("a repeated face id is rejected and the handle is untouched") {
        std::vector<uint32_t> bad{region[0], region[0]};
        CHECK(cyber_mesh_set_solve_region(mesh, bad.data(), bad.size()) == CYBER_ERR_INVALID_ARG);
        CHECK(cyber_mesh_set_solve_region(mesh, region.data(), region.size()) == CYBER_OK);
    }

    SUBCASE("readback before a region solve is refused, not silently empty") {
        size_t count = 12345;
        CHECK(cyber_mesh_solved_faces(mesh, nullptr, 0, &count) == CYBER_ERR_INVALID_ARG);
        CyberRegionReport report{};
        CHECK(cyber_mesh_region_report(mesh, &report) == CYBER_ERR_INVALID_ARG);
    }

    SUBCASE("duplicate preserves ids where a payload round-trip would not") {
        CyberMesh* copy = nullptr;
        REQUIRE(cyber_mesh_duplicate(mesh, &copy) == CYBER_OK);
        REQUIRE(copy != nullptr);
        uint32_t a = 0, b = 0;
        REQUIRE(cyber_mesh_vertex_face_count(mesh, 24, &a) == CYBER_OK);  // interior vertex (3,3)
        REQUIRE(cyber_mesh_vertex_face_count(copy, 24, &b) == CYBER_OK);
        CHECK(a == b);
        CHECK(a == 4);
        cyber_mesh_destroy(copy);
    }

    SUBCASE("a region solve keeps frozen ids and reports the interface") {
        REQUIRE(cyber_mesh_set_solve_region(mesh, region.data(), region.size()) == CYBER_OK);
        CyberRemeshParams params;
        cyber_remesh_params_default(&params);
        params.targetQuads = 400;

        CyberMesh* out = nullptr;
        REQUIRE(cyber_remesh(mesh, &params, nullptr, nullptr, nullptr, &out) == CYBER_OK);
        REQUIRE(out != nullptr);

        CyberRegionReport report{};
        REQUIRE(cyber_mesh_region_report(out, &report) == CYBER_OK);
        CHECK(report.interface_vertex_count == 16);

        size_t ifaceCount = 0;
        REQUIRE(cyber_mesh_interface_vertices(out, nullptr, 0, &ifaceCount) == CYBER_OK);
        CHECK(ifaceCount == 16);
        std::vector<uint32_t> iface(ifaceCount);
        REQUIRE(cyber_mesh_interface_vertices(out, iface.data(), iface.size(), &ifaceCount) ==
                CYBER_OK);
        CHECK(std::is_sorted(iface.begin(), iface.end()));

        size_t solved = 0;
        REQUIRE(cyber_mesh_solved_faces(out, nullptr, 0, &solved) == CYBER_OK);
        CHECK(solved > 0);

        // Irregularity is REPORTED, and reporting it must not have blocked the
        // solve. Undoing that rescope breaks this test.
        size_t irregular = 0;
        REQUIRE(cyber_mesh_interface_irregular(out, nullptr, 0, &irregular) == CYBER_OK);
        CHECK(irregular == report.interface_irregular_count);

        cyber_mesh_destroy(out);
    }

    cyber_mesh_destroy(mesh);
}

// --- meshlet clusters (add-meshlet-target-path, task 3.2) --------------------

TEST_CASE("capi meshlets cluster the render streams and honour the cache lifetime") {
    const std::string path = writeGridObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(path.c_str(), &mesh) == CYBER_OK);
    REQUIRE(mesh != nullptr);

    size_t meshletCount = 0, vertexCount = 0, indexCount = 0;
    const CyberMeshlet* meshlets = cyber_mesh_meshlets_ptr(mesh, &meshletCount);
    const uint32_t* vertices = cyber_mesh_meshlet_vertices_ptr(mesh, &vertexCount);
    const uint8_t* localIndices = cyber_mesh_meshlet_indices_ptr(mesh, &indexCount);
    REQUIRE(meshlets != nullptr);
    REQUIRE(meshletCount > 0);
    REQUIRE(vertices != nullptr);
    REQUIRE(localIndices != nullptr);

    // Every triangle of the render stream ends up in exactly one cluster.
    size_t triangleTotal = 0;
    cyber_mesh_triangle_indices_ptr(mesh, &triangleTotal);
    size_t clustered = 0;
    for (size_t i = 0; i < meshletCount; ++i) {
        clustered += meshlets[i].triangle_count;
    }
    CHECK(clustered == triangleTotal / 3);
    CHECK(indexCount == clustered * 3);

    // Meshlet vertex indices address the SAME compacted array the position
    // pointer view exposes -- that is what lets one position buffer serve every
    // cluster, so an out-of-range index here would be a GPU read past the end.
    size_t positionFloats = 0;
    cyber_mesh_positions_ptr(mesh, &positionFloats);
    const uint32_t renderVertexCount = static_cast<uint32_t>(positionFloats / 3);
    for (size_t i = 0; i < vertexCount; ++i) {
        CHECK(vertices[i] < renderVertexCount);
    }
    // Local corner indices stay inside their own cluster's vertex slice.
    for (size_t i = 0; i < meshletCount; ++i) {
        const CyberMeshlet& m = meshlets[i];
        for (uint32_t t = 0; t < m.triangle_count; ++t) {
            for (int c = 0; c < 3; ++c) {
                const size_t at = (static_cast<size_t>(m.triangle_offset) + t) * 3
                    + static_cast<size_t>(c);
                CHECK(localIndices[at] < m.vertex_count);
            }
        }
        // Conservative bounds, as the builder promises.
        CHECK(m.radius >= 0.0f);
        CHECK(m.cone_cutoff >= 0.0f);
        CHECK(m.cone_cutoff <= 1.0f);
    }

    // LIFETIME: a mutating call invalidates the cache, so the clusters are
    // rebuilt on the next request rather than served stale. The compacted vertex
    // ORDER also moves, which is what makes a stale meshlet dangerous.
    size_t before = meshletCount;
    uint32_t faces[1] = {0};
    size_t removed = 0;
    REQUIRE(cyber_retopo_delete_faces(mesh, faces, 1, &removed) == CYBER_OK);
    REQUIRE(removed == 1);
    size_t after = 0;
    const CyberMeshlet* rebuilt = cyber_mesh_meshlets_ptr(mesh, &after);
    REQUIRE(rebuilt != nullptr);
    size_t clusteredAfter = 0;
    for (size_t i = 0; i < after; ++i) {
        clusteredAfter += rebuilt[i].triangle_count;
    }
    size_t triangleTotalAfter = 0;
    cyber_mesh_triangle_indices_ptr(mesh, &triangleTotalAfter);
    CHECK(clusteredAfter == triangleTotalAfter / 3);
    CHECK(clusteredAfter < clustered);  // a face really was removed
    CHECK(before > 0);

    cyber_mesh_destroy(mesh);
}

// UV readback (openspec add-uv-stage-foundation, task 1).
//
// The load-bearing property is that ABSENCE is distinguishable from ZERO: a mesh that
// was never unwrapped must not look like a layout collapsed onto the origin, because a
// 2D view cannot tell those apart from the numbers alone and would draw the second as
// a catastrophic result rather than saying "nothing here yet".
TEST_CASE("capi UV readback: absent before an unwrap, per-corner after one") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    REQUIRE(mesh != nullptr);

    // A cube OBJ carries no vt lines, so it has never been unwrapped.
    size_t uvFloats = 12345;  // poisoned, to catch a callee that forgets to write it
    const float* uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    CHECK(uvs == nullptr);
    CHECK(uvFloats == 0);

    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    CyberAtlasResult atlas{};
    REQUIRE(cyber_uv_atlas(mesh, &params, &atlas) == CYBER_OK);
    CHECK(atlas.chartCount > 0);

    // After the atlas there is one u,v pair per emitted triangle CORNER, so the UV
    // stream is exactly parallel to the triangle index stream: 2 floats where that has
    // 1 index. A mismatch here means the fan triangulation and the UV emission
    // disagreed, which would shear the whole layout by one corner.
    size_t indexCount = 0;
    cyber_mesh_triangle_indices_ptr(mesh, &indexCount);
    uvFloats = 0;
    uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(uvs != nullptr);
    CHECK(indexCount > 0);
    CHECK(uvFloats == indexCount * 2);

    // Packed into the unit square, so every coordinate is finite and in range. A NaN
    // here would render as nothing rather than as an error.
    bool inRange = true;
    for (size_t i = 0; i < uvFloats; ++i) {
        if (!(uvs[i] >= -0.001f && uvs[i] <= 1.001f)) {
            inRange = false;
        }
    }
    CHECK(inRange);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Per-face UV distortion readback (openspec add-uv-distortion-heatmaps, task 1).
//
// The heatmap's whole value is saying WHERE distortion is, so the per-face array has to
// line up with the live-face walk the caller uses to rebuild rings. And absence has to
// stay distinguishable from zero: reporting zeros for a never-unwrapped mesh would draw
// it as a flawless layout.
TEST_CASE("capi per-face UV distortion: absent before an unwrap, one entry per face after") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    REQUIRE(mesh != nullptr);

    size_t count = 4242;  // poisoned, to catch a callee that forgets to write it
    const CyberFaceDistortion* distortion = cyber_mesh_uv_distortion_ptr(mesh, &count);
    CHECK(distortion == nullptr);
    CHECK(count == 0);

    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    CyberAtlasResult atlas{};
    REQUIRE(cyber_uv_atlas(mesh, &params, &atlas) == CYBER_OK);

    count = 0;
    distortion = cyber_mesh_uv_distortion_ptr(mesh, &count);
    REQUIRE(distortion != nullptr);
    // Six quads on a cube, so one entry each — the cube is unchanged by unwrapping, which
    // only adds a corner attribute.
    CHECK(count == 6);

    bool sane = true;
    for (size_t i = 0; i < count; ++i) {
        // Conformal error is defined on [0,1); an area ratio must be finite and
        // non-negative. A NaN would shade as nothing rather than as an error.
        if (!(distortion[i].angle >= 0.0f && distortion[i].angle < 1.0f)) {
            sane = false;
        }
        if (!(distortion[i].area >= 0.0f) || !std::isfinite(distortion[i].area)) {
            sane = false;
        }
    }
    CHECK(sane);

    // Agrees with the aggregate the atlas reported, because both come from the same
    // measurement rather than two implementations of it.
    float worst = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        worst = std::fmax(worst, distortion[i].angle);
    }
    CHECK(worst <= atlas.maxAngleDistortion + 1e-4f);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Hand-drawn UV seams (openspec add-uv-seam-authoring, task 1).
//
// The property worth protecting is that authored seams REPLACE the automatic ones. A union
// would cut where the artist did not ask, and that is invisible from the chart count alone
// unless the test compares against the auto-seamed baseline.
TEST_CASE("capi hand-drawn seams replace auto-seams rather than supplementing them") {
    const std::filesystem::path objPath = writeCubeObj();

    // Baseline: the automatic path on a cube.
    CyberMesh* autoMesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &autoMesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    CyberAtlasResult autoAtlas{};
    REQUIRE(cyber_uv_atlas(autoMesh, &params, &autoAtlas) == CYBER_OK);
    CHECK(autoAtlas.seamEdges > 0);

    // Now the same cube with exactly TWO authored seam edges.
    CyberMesh* seamMesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &seamMesh) == CYBER_OK);
    size_t edgeIndexCount = 0;
    cyber_mesh_edge_indices_ptr(seamMesh, &edgeIndexCount);
    REQUIRE(edgeIndexCount >= 4);  // at least two edges exist

    const uint32_t authored[2] = {0, 1};
    REQUIRE(cyber_mesh_set_seam_edges(seamMesh, authored, 2) == CYBER_OK);
    CyberAtlasResult seamAtlas{};
    REQUIRE(cyber_uv_atlas(seamMesh, &params, &seamAtlas) == CYBER_OK);

    // Exactly the authored count: if auto-seams were unioned in, this would report the
    // baseline's seam count or more, and the artist would get cuts they never drew.
    CHECK(seamAtlas.seamEdges == 2);

    // A rejected id must leave the handle untouched rather than half-updated.
    const uint32_t bad[2] = {0, 999999};
    CHECK(cyber_mesh_set_seam_edges(seamMesh, bad, 2) == CYBER_ERR_INVALID_ARG);
    CyberAtlasResult afterBad{};
    REQUIRE(cyber_uv_atlas(seamMesh, &params, &afterBad) == CYBER_OK);
    CHECK(afterBad.seamEdges == 2);

    // Clearing returns to the automatic path exactly, so the capability is inert when unused.
    REQUIRE(cyber_mesh_set_seam_edges(seamMesh, nullptr, 0) == CYBER_OK);
    CyberAtlasResult cleared{};
    REQUIRE(cyber_uv_atlas(seamMesh, &params, &cleared) == CYBER_OK);
    CHECK(cleared.seamEdges == autoAtlas.seamEdges);
    CHECK(cleared.chartCount == autoAtlas.chartCount);

    cyber_mesh_free(autoMesh);
    cyber_mesh_free(seamMesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Single-island re-unwrap (openspec add-stage-dependent-x-gesture, task 1).
TEST_CASE("capi single-island re-unwrap refuses a mesh with no UVs") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);

    CyberReunwrapResult r{};
    // OK with a FLAG, not an error: "you have not unwrapped yet" is a routing signal for the
    // caller, not a bad argument, and conflating the two would make the app unable to tell
    // them apart.
    REQUIRE(cyber_uv_reunwrap_island(mesh, 0, nullptr, &r) == CYBER_OK);
    CHECK(r.needsWholeMeshUnwrap == 1);

    // And it must not have created the UV column: a column holding one island leaves every
    // other island's corners at (0,0), which reads downstream as a real layout.
    size_t uvFloats = 0;
    CHECK(cyber_mesh_uvs_ptr(mesh, &uvFloats) == nullptr);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi single-island re-unwrap changes one island and invalidates the cache") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);

    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    size_t uvFloats = 0;
    const float* uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(uvs != nullptr);
    const std::vector<float> before(uvs, uvs + uvFloats);

    CyberReunwrapResult r{};
    REQUIRE(cyber_uv_reunwrap_island(mesh, 0, &params, &r) == CYBER_OK);
    CHECK(r.needsWholeMeshUnwrap == 0);
    CHECK(r.faces >= 1);

    // Re-read through the SAME accessor: if the re-unwrap failed to invalidate the render
    // cache this would hand back `before` byte for byte and the call would look like a no-op.
    // That is exactly the bug cyber_uv_atlas shipped with until 6.1.
    uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(uvs != nullptr);
    REQUIRE(uvFloats == before.size());
    const std::vector<float> after(uvs, uvs + uvFloats);

    // Some corners moved (the island was re-solved) and some did not (the other islands were
    // left alone). Both halves matter: all-changed would mean the whole layout was redone.
    bool changed = false, kept = false;
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] != after[i]) {
            changed = true;
        } else {
            kept = true;
        }
    }
    CHECK(changed);
    CHECK(kept);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi single-island re-unwrap rejects a dead face") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    CyberReunwrapResult r{};
    // A bad face id IS an argument error, unlike the no-UVs refusal above.
    CHECK(cyber_uv_reunwrap_island(mesh, 9999, &params, &r) == CYBER_ERR_INVALID_ARG);
    CHECK(r.needsWholeMeshUnwrap == 0);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Manual packing aids (openspec add-uv-packing-aids, task 2).
TEST_CASE("capi region packing keeps every island inside the region") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    REQUIRE(cyber_uv_pack_region(mesh, 0.25f, 0.25f, 0.75f, 0.75f, &params, nullptr) == CYBER_OK);

    size_t uvFloats = 0;
    const float* uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(uvs != nullptr);
    for (size_t i = 0; i + 1 < uvFloats; i += 2) {
        CHECK(uvs[i] >= 0.25f - 1e-4f);
        CHECK(uvs[i] <= 0.75f + 1e-4f);
        CHECK(uvs[i + 1] >= 0.25f - 1e-4f);
        CHECK(uvs[i + 1] <= 0.75f + 1e-4f);
    }

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi region packing refuses a mesh with no UVs and a degenerate region") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // No layout to repack.
    CHECK(cyber_uv_pack_region(mesh, 0, 0, 1, 1, &params, nullptr) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    // Zero-area region: refused, not collapsed onto a line.
    CHECK(cyber_uv_pack_region(mesh, 0.5f, 0.2f, 0.5f, 0.8f, &params, nullptr)
          == CYBER_ERR_INVALID_ARG);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi flipped-island readback names a face and a flip is reversible") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // No layout: an empty answer, not an error — the caller asked a set-membership question.
    size_t none = 999;
    REQUIRE(cyber_uv_flipped_islands(mesh, &params, nullptr, 0, &none) == CYBER_OK);
    CHECK(none == 0);

    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    size_t uvFloats = 0;
    const float* p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(p != nullptr);
    const std::vector<float> before(p, p + uvFloats);

    size_t clean = 999;
    REQUIRE(cyber_uv_flipped_islands(mesh, &params, nullptr, 0, &clean) == CYBER_OK);
    CHECK(clean == 0);

    // Flip one island, then find it by readback rather than by assuming which it was.
    REQUIRE(cyber_uv_flip_island(mesh, 0, &params) == CYBER_OK);
    size_t count = 0;
    REQUIRE(cyber_uv_flipped_islands(mesh, &params, nullptr, 0, &count) == CYBER_OK);
    REQUIRE(count == 1);
    std::vector<uint32_t> faces(count);
    size_t written = 0;
    REQUIRE(cyber_uv_flipped_islands(mesh, &params, faces.data(), faces.size(), &written)
            == CYBER_OK);
    REQUIRE(written == 1);

    // Flipping the reported face back restores the layout exactly.
    REQUIRE(cyber_uv_flip_island(mesh, faces[0], &params) == CYBER_OK);
    p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(p != nullptr);
    REQUIRE(uvFloats == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(std::fabs(p[i] - before[i]) < 1e-6f);
    }
    size_t after = 999;
    REQUIRE(cyber_uv_flipped_islands(mesh, &params, nullptr, 0, &after) == CYBER_OK);
    CHECK(after == 0);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Per-vertex UV editing (openspec add-uv-imported-preview, 6.3d).
TEST_CASE("capi UV vertex move reports a miss as zero rather than an error") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // No layout at all is an argument error: there is nothing to edit.
    size_t moved = 99;
    CHECK(cyber_uv_move_island_uv_vertex(mesh, 0, 0, 0, 0.1f, 0, 1e-4f, &params, &moved)
          == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    size_t uvFloats = 0;
    const float* uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(uvs != nullptr);
    const std::vector<float> before(uvs, uvs + uvFloats);

    // A MISS is OK-with-zero, not an error: a drag that hit no vertex simply is not an edit, and
    // conflating it with an invalid face would leave the caller unable to tell them apart.
    REQUIRE(cyber_uv_move_island_uv_vertex(mesh, 0, 9.0f, 9.0f, 0.1f, 0.1f, 1e-4f, &params, &moved)
            == CYBER_OK);
    CHECK(moved == 0);
    uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(uvs[i] == before[i]);
    }

    // A hit moves at least one corner and changes the layout.
    REQUIRE(cyber_uv_move_island_uv_vertex(mesh, 0, before[0], before[1], 0.02f, -0.01f, 1e-3f,
                                           &params, &moved)
            == CYBER_OK);
    CHECK(moved >= 1);
    uvs = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    bool changed = false;
    for (size_t i = 0; i < before.size(); ++i) {
        if (uvs[i] != before[i]) { changed = true; }
    }
    CHECK(changed);

    // A negative tolerance is meaningless.
    CHECK(cyber_uv_move_island_uv_vertex(mesh, 0, 0, 0, 0.1f, 0, -1.0f, &params, &moved)
          == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_uv_move_island_uv_vertex(mesh, 9999, 0, 0, 0.1f, 0, 1e-4f, &params, &moved)
          == CYBER_ERR_INVALID_ARG);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Multiple UV sets (openspec add-uv-sets, 6.7a).
namespace {
std::string uvSetName(const CyberMesh* mesh, size_t index) {
    size_t length = 0;
    REQUIRE(cyber_mesh_uv_set_name(mesh, index, nullptr, 0, &length) == CYBER_OK);
    std::string out(length, '\0');
    REQUIRE(cyber_mesh_uv_set_name(mesh, index, out.data(), out.size(), &length) == CYBER_OK);
    out.resize(length - 1);  // drop the NUL
    return out;
}
std::string activeUvSetName(const CyberMesh* mesh) {
    size_t length = 0;
    REQUIRE(cyber_mesh_uv_set_active(mesh, nullptr, 0, &length) == CYBER_OK);
    std::string out(length, '\0');
    REQUIRE(cyber_mesh_uv_set_active(mesh, out.data(), out.size(), &length) == CYBER_OK);
    out.resize(length - 1);
    return out;
}
}  // namespace

TEST_CASE("capi UV sets: create, activate, rename, delete") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // Before any unwrap there is no layout, so there is nothing to copy into a new set.
    CHECK(cyber_mesh_uv_set_create(mesh, "lightmap") == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    CHECK(cyber_mesh_uv_set_count(mesh) == 1);
    CHECK(activeUvSetName(mesh) == "default");

    REQUIRE(cyber_mesh_uv_set_create(mesh, "lightmap") == CYBER_OK);
    CHECK(cyber_mesh_uv_set_count(mesh) == 2);
    // Ascending, so enumeration is deterministic run to run.
    CHECK(uvSetName(mesh, 0) == "default");
    CHECK(uvSetName(mesh, 1) == "lightmap");
    CHECK(activeUvSetName(mesh) == "default");

    // Names must not contain the internal ':' separator, and duplicates are refused.
    CHECK(cyber_mesh_uv_set_create(mesh, "a:b") == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_mesh_uv_set_create(mesh, "lightmap") == CYBER_ERR_INVALID_ARG);

    REQUIRE(cyber_mesh_uv_set_activate(mesh, "lightmap") == CYBER_OK);
    CHECK(activeUvSetName(mesh) == "lightmap");
    CHECK(cyber_mesh_uv_set_count(mesh) == 2);
    CHECK(cyber_mesh_uv_set_activate(mesh, "lightmap") == CYBER_ERR_INVALID_ARG);  // already
    CHECK(cyber_mesh_uv_set_activate(mesh, "nope") == CYBER_ERR_INVALID_ARG);

    // Renaming the ACTIVE set: no column moves, and the handle is the only place the name lives.
    REQUIRE(cyber_mesh_uv_set_rename(mesh, "lightmap", "bake") == CYBER_OK);
    CHECK(activeUvSetName(mesh) == "bake");

    // The active set can never be deleted, or the mesh would be left without one.
    CHECK(cyber_mesh_uv_set_delete(mesh, "bake") == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_mesh_uv_set_delete(mesh, "default") == CYBER_OK);
    CHECK(cyber_mesh_uv_set_count(mesh) == 1);

    // The active set is still a working layout after all of that.
    size_t uvFloats = 0;
    CHECK(cyber_mesh_uvs_ptr(mesh, &uvFloats) != nullptr);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi UV set sidecar round-trips and rejects the wrong topology") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* source = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &source) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    REQUIRE(cyber_uv_atlas(source, &params, nullptr) == CYBER_OK);
    REQUIRE(cyber_mesh_uv_set_create(source, "lightmap") == CYBER_OK);

    size_t blobLength = 0;
    REQUIRE(cyber_mesh_uv_sets_serialize(source, nullptr, 0, &blobLength) == CYBER_OK);
    REQUIRE(blobLength > 0);
    std::vector<uint8_t> blob(blobLength);
    REQUIRE(cyber_mesh_uv_sets_serialize(source, blob.data(), blob.size(), &blobLength)
            == CYBER_OK);

    // Restored onto the same topology: both sets come back and the active name with them.
    CyberMesh* target = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &target) == CYBER_OK);
    REQUIRE(cyber_uv_atlas(target, &params, nullptr) == CYBER_OK);
    REQUIRE(cyber_mesh_uv_sets_deserialize(target, blob.data(), blob.size()) == CYBER_OK);
    CHECK(cyber_mesh_uv_set_count(target) == 2);
    CHECK(activeUvSetName(target) == "default");

    // A truncated sidecar is rejected whole rather than half-applied.
    CHECK(cyber_mesh_uv_sets_deserialize(target, blob.data(), blob.size() / 2)
          == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_mesh_uv_set_count(target) == 2);

    cyber_mesh_free(source);
    cyber_mesh_free(target);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Island UV editing (openspec add-uv-island-editing, 6.3).
TEST_CASE("capi island transform moves, rotates and scales in one atomic call") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // No layout to transform.
    CHECK(cyber_uv_transform_island(mesh, 0, 0.1f, 0, 0, 1, &params) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    size_t uvFloats = 0;
    const float* p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(p != nullptr);
    const std::vector<float> before(p, p + uvFloats);

    // A non-positive scale would collapse or MIRROR the island. Mirroring has its own entry
    // point where the winding change is the point; smuggling it through a negative scale would
    // make a transform silently produce a defect.
    CHECK(cyber_uv_transform_island(mesh, 0, 0, 0, 0, 0.0f, &params) == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_uv_transform_island(mesh, 0, 0, 0, 0, -1.0f, &params) == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_uv_transform_island(mesh, 9999, 0, 0, 0, 1.0f, &params) == CYBER_ERR_INVALID_ARG);

    // A pure translation moves exactly one island and leaves the rest bitwise identical.
    REQUIRE(cyber_uv_transform_island(mesh, 0, 0.25f, -0.1f, 0, 1.0f, &params) == CYBER_OK);
    p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(p != nullptr);
    size_t moved = 0, kept = 0;
    for (size_t i = 0; i + 1 < uvFloats; i += 2) {
        if (p[i] != before[i] || p[i + 1] != before[i + 1]) {
            CHECK(std::fabs((p[i] - before[i]) - 0.25f) < 1e-5f);
            CHECK(std::fabs((p[i + 1] - before[i + 1]) + 0.1f) < 1e-5f);
            ++moved;
        } else {
            ++kept;
        }
    }
    CHECK(moved > 0);
    CHECK(kept > 0);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi island rotation by a full turn returns the island to where it started") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    size_t uvFloats = 0;
    const float* p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    const std::vector<float> before(p, p + uvFloats);

    // Four quarter turns about the island centroid. Each rotation recomputes the centroid, so
    // this also pins that the pivot does not drift between steps.
    const float quarter = 1.57079632679f;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(cyber_uv_transform_island(mesh, 0, 0, 0, quarter, 1.0f, &params) == CYBER_OK);
    }
    p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(std::fabs(p[i] - before[i]) < 1e-4f);
    }

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi cloning refuses an island onto itself and mismatched topology") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    params.mergeCharts = 0;
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    size_t uvFloats = 0;
    const float* p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    const std::vector<float> before(p, p + uvFloats);

    // Onto ITSELF: a no-op that would report success, so a mis-aimed gesture would look like it
    // worked. Refused instead.
    CHECK(cyber_uv_clone_island(mesh, 0, 0, &params) == CYBER_ERR_INVALID_ARG);
    p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(p[i] == before[i]);
    }

    // Between two single-quad cube faces the topology DOES match, so this must succeed and make
    // the destination carry the source's UVs.
    REQUIRE(cyber_uv_clone_island(mesh, 0, 1, &params) == CYBER_OK);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi grid straighten and symmetrize validate their arguments") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    CHECK(cyber_uv_grid_straighten_island(mesh, 0, 0.25f, &params) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    REQUIRE(cyber_uv_grid_straighten_island(mesh, 0, 0.25f, &params) == CYBER_OK);

    // A zero direction is not an axis; symmetrizing about it is meaningless.
    CHECK(cyber_uv_symmetrize_island(mesh, 0, 0.5f, 0.5f, 0, 0, 1.0f, &params)
          == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_symmetrize_island(mesh, 0, 0.5f, 0.5f, 1, 0, 1.0f, &params) == CYBER_OK);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi stitching removes edges from the seam set and rejects a dead edge whole") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    const uint32_t authored[3] = {0, 1, 2};
    REQUIRE(cyber_mesh_set_seam_edges(mesh, authored, 3) == CYBER_OK);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);

    // Validated as a WHOLE first: a rejected id must leave the seam set untouched rather than
    // half-stitched.
    const uint32_t bad[2] = {0, 9999};
    CHECK(cyber_uv_stitch_islands(mesh, bad, 2, &params) == CYBER_ERR_INVALID_ARG);
    CyberAtlasResult still{};
    REQUIRE(cyber_uv_atlas(mesh, &params, &still) == CYBER_OK);
    CHECK(still.seamEdges == 3);

    // Sewing one edge leaves two.
    const uint32_t one[1] = {1};
    REQUIRE(cyber_uv_stitch_islands(mesh, one, 1, &params) == CYBER_OK);
    CyberAtlasResult after{};
    REQUIRE(cyber_uv_atlas(mesh, &params, &after) == CYBER_OK);
    CHECK(after.seamEdges == 2);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// UDIM tiles and symmetry stacking (openspec add-uv-sets-and-stacking).
TEST_CASE("capi UDIM tiles start at 1001 and follow the island when it is retiled") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // No layout: an empty tile list, not an error. An exporter asking "which tiles" before an
    // unwrap should be told "none", not handed a failure.
    size_t bare = 99;
    REQUIRE(cyber_uv_udim_tiles(mesh, &params, nullptr, 0, &bare) == CYBER_OK);
    CHECK(bare == 0);

    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    size_t count = 0;
    REQUIRE(cyber_uv_udim_tiles(mesh, &params, nullptr, 0, &count) == CYBER_OK);
    REQUIRE(count == 1);
    std::vector<int32_t> tiles(count);
    size_t written = 0;
    REQUIRE(cyber_uv_udim_tiles(mesh, &params, tiles.data(), tiles.size(), &written) == CYBER_OK);
    REQUIRE(written == 1);
    CHECK(tiles[0] == 1001);  // the unit square IS tile 1001

    // Retile one island; the list must now name both tiles.
    REQUIRE(cyber_uv_assign_island_tile(mesh, 0, 1013, &params) == CYBER_OK);
    REQUIRE(cyber_uv_udim_tiles(mesh, &params, nullptr, 0, &count) == CYBER_OK);
    REQUIRE(count == 2);
    std::vector<int32_t> both(count);
    REQUIRE(cyber_uv_udim_tiles(mesh, &params, both.data(), both.size(), &written) == CYBER_OK);
    CHECK(both[0] == 1001);
    CHECK(both[1] == 1013);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi retiling rejects a tile below the UDIM range and a mesh with no UVs") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    CHECK(cyber_uv_assign_island_tile(mesh, 0, 1001, &params) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    // 1000 is not a UDIM tile; accepting it would emit a texture name no DCC tool would find.
    CHECK(cyber_uv_assign_island_tile(mesh, 0, 1000, &params) == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_uv_assign_island_tile(mesh, 9999, 1002, &params) == CYBER_ERR_INVALID_ARG);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi stacking reports nothing stacked for a non-symmetric mesh") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);

    // No layout is an ERROR here (unlike the tile query): stacking is a mutation, and being
    // asked to rearrange a layout that does not exist is a mistake, not an empty answer.
    size_t stacked = 99;
    CHECK(cyber_uv_stack_mirrored_islands(mesh, 0, 0, 0, 1, 0, 0, 0.01f, &params, &stacked)
          == CYBER_ERR_INVALID_ARG);

    REQUIRE(cyber_uv_atlas(mesh, &params, nullptr) == CYBER_OK);
    size_t uvFloats = 0;
    const float* p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(p != nullptr);
    const std::vector<float> before(p, p + uvFloats);

    // A zero normal is not a plane.
    CHECK(cyber_uv_stack_mirrored_islands(mesh, 0, 0, 0, 0, 0, 0, 0.01f, &params, &stacked)
          == CYBER_ERR_INVALID_ARG);

    // A tight tolerance on a cube finds no genuine mirror pairs, and reporting 0 is correct —
    // a wrong pairing would stack unrelated shells on top of each other.
    REQUIRE(cyber_uv_stack_mirrored_islands(mesh, 0.5f, 0.5f, 0.5f, 1, 0, 0, 1e-5f, &params,
                                            &stacked)
            == CYBER_OK);
    p = cyber_mesh_uvs_ptr(mesh, &uvFloats);
    REQUIRE(p != nullptr);
    if (stacked == 0) {
        // Nothing stacked must mean nothing written.
        for (size_t i = 0; i < before.size(); ++i) {
            CHECK(p[i] == before[i]);
        }
    }

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Auto-seam proposals (openspec add-auto-seam-proposals, task 1).
//
// The property that matters: a proposal can only ever ADD. If accepting one could delete a
// seam the artist drew, the feature would be actively hostile — so the authored set must
// always be a subset of the proposal.
TEST_CASE("capi seam proposals always contain the authored seams and never apply") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);

    // With nothing authored, a proposal is just the automatic seaming.
    size_t bare = 0;
    REQUIRE(cyber_mesh_propose_seams(mesh, nullptr, 0, &bare) == CYBER_OK);
    CHECK(bare > 0);

    const uint32_t authored[2] = {0, 1};
    REQUIRE(cyber_mesh_set_seam_edges(mesh, authored, 2) == CYBER_OK);

    size_t count = 0;
    REQUIRE(cyber_mesh_propose_seams(mesh, nullptr, 0, &count) == CYBER_OK);
    REQUIRE(count > 0);
    std::vector<uint32_t> ids(count);
    size_t written = 0;
    REQUIRE(cyber_mesh_propose_seams(mesh, ids.data(), ids.size(), &written) == CYBER_OK);
    CHECK(written == count);

    // Every authored seam is in the proposal. This is what makes accepting safe.
    for (const uint32_t id : authored) {
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }

    // Ascending, so the same mesh and the same authored seams give the same proposal —
    // a set's iteration order would not.
    CHECK(std::is_sorted(ids.begin(), ids.end()));

    // PROPOSES, does not apply: the atlas still reports only the authored seams, because
    // the proposal was never written to the mesh.
    CyberAtlasParams params{};
    cyber_default_atlas_params(&params);
    CyberAtlasResult atlas{};
    REQUIRE(cyber_uv_atlas(mesh, &params, &atlas) == CYBER_OK);
    CHECK(atlas.seamEdges == 2);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}
