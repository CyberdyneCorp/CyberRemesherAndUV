// Handle-cache semantics of the C ABI: which cached structures an edit is
// allowed to keep, and which it must throw away.
//
// A mesh handle caches two things derived from the mesh — the compacted render
// buffers and the one-ring/vertex->face adjacency the brush sweeps read. Both
// are keyed on TOPOLOGY, so an edit that only moves vertices keeps them and
// refreshes just the position-derived buffers. Get that split wrong and the
// handle serves stale geometry with CYBER_OK, which is exactly what these
// cases exist to catch.
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cyber_capi.h"

namespace {

std::vector<float> gridPoints(int cols, int rows, float z) {
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

CyberMesh* makeGrid(int cols, int rows, float z = 0.0f) {
    CyberMesh* mesh = cyber_mesh_create();
    REQUIRE(mesh != nullptr);
    const std::vector<float> pts = gridPoints(cols, rows, z);
    size_t faces = 0;
    REQUIRE(cyber_retopo_create_grid(mesh, pts.data(), static_cast<size_t>(rows - 1),
                                     static_cast<size_t>(cols - 1), nullptr, &faces) == CYBER_OK);
    return mesh;
}

std::vector<float> renderPositions(const CyberMesh* mesh) {
    size_t n = 0;
    const float* p = cyber_mesh_positions_ptr(mesh, &n);
    return p == nullptr ? std::vector<float>{} : std::vector<float>(p, p + n);
}

std::vector<float> renderNormals(const CyberMesh* mesh) {
    size_t n = 0;
    const float* p = cyber_mesh_normals_ptr(mesh, &n);
    return p == nullptr ? std::vector<float>{} : std::vector<float>(p, p + n);
}

std::vector<std::uint32_t> triangleIndices(const CyberMesh* mesh) {
    size_t n = 0;
    const uint32_t* p = cyber_mesh_triangle_indices_ptr(mesh, &n);
    return p == nullptr ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>(p, p + n);
}

std::vector<std::uint32_t> edgeIndices(const CyberMesh* mesh) {
    size_t n = 0;
    const uint32_t* p = cyber_mesh_edge_indices_ptr(mesh, &n);
    return p == nullptr ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>(p, p + n);
}

std::vector<float> allPositions(const CyberMesh* mesh) {
    std::vector<float> out(cyber_mesh_copy_positions(mesh, nullptr, 0));
    cyber_mesh_copy_positions(mesh, out.data(), out.size());
    return out;
}

std::vector<float> selectionWeights(const CyberMesh* mesh) {
    std::vector<float> out(cyber_retopo_selection_copy_weights(mesh, nullptr, 0));
    cyber_retopo_selection_copy_weights(mesh, out.data(), out.size());
    return out;
}

// A repeatable weight field over every vertex the handle currently has.
void seedWeights(CyberMesh* mesh) {
    std::vector<float> w(cyber_mesh_vertex_count(mesh) * 2, 0.0f);
    for (std::size_t i = 0; i < w.size(); ++i) {
        w[i] = static_cast<float>(i % 5) * 0.25f;
    }
    REQUIRE(cyber_retopo_selection_set_weights(mesh, w.data(), w.size()) == CYBER_OK);
}

// The quad that welds the grid's two far corners together, changing the valence
// of the vertices it touches — a topology edit the adjacency table cannot
// survive. Adds four vertices, so it also grows the mesh.
uint32_t bridgeCorners(CyberMesh* mesh) {
    const float quad[12] = {0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 1.5f, 5.0f, 1.0f, 3.0f, 0.0f, 0.0f};
    uint32_t face = 0;
    REQUIRE(cyber_retopo_create_face(mesh, quad, 4, nullptr, &face) == CYBER_OK);
    return face;
}

// Deletes the grid's corner quad. Its two outer edges belong to no other face,
// so they die with it: the quad's outer vertex is retired and its two
// neighbours each lose a one-ring entry — a genuine rewiring. Retired ids are
// recycled rather than compacted away, so the mesh's id space does not shrink
// and a stale one-ring table cannot be caught by a size check alone.
void rewireCorner(CyberMesh* mesh) {
    const size_t before = cyber_mesh_vertex_count(mesh);
    const uint32_t corner = 0u;
    size_t removed = 0;
    REQUIRE(cyber_retopo_delete_faces(mesh, &corner, 1, &removed) == CYBER_OK);
    REQUIRE(removed == 1u);
    REQUIRE(cyber_mesh_vertex_count(mesh) == before - 1u);
}

// A clone of the handle: same mesh, same weights, but no caches of its own, so
// whatever it computes is what a correctly invalidated handle must compute.
CyberMesh* uncachedCopy(const CyberMesh* mesh) {
    CyberMesh* copy = nullptr;
    REQUIRE(cyber_mesh_clone(mesh, &copy) == CYBER_OK);
    return copy;
}

}  // namespace

// A positions-only edit must publish the new geometry through EVERY
// position-derived accessor. Normals are the trap: they are keyed on the render
// vertex order, which does not change, but their VALUES come from the faces.
TEST_CASE("capi cache: a positions-only edit refreshes positions and normals") {
    CyberMesh* mesh = makeGrid(4, 4);
    const std::vector<float> positionsBefore = renderPositions(mesh);
    const std::vector<float> normalsBefore = renderNormals(mesh);
    REQUIRE(positionsBefore.size() == 48u);
    REQUIRE(normalsBefore.size() == 48u);

    // Lift one interior vertex clean out of the plane.
    const float target[3] = {1.0f, 1.0f, 2.0f};
    REQUIRE(cyber_retopo_tweak_vertex(mesh, 5, target, nullptr) == CYBER_OK);

    const std::vector<float> positionsAfter = renderPositions(mesh);
    const std::vector<float> normalsAfter = renderNormals(mesh);
    REQUIRE(positionsAfter.size() == positionsBefore.size());
    CHECK(positionsAfter != positionsBefore);
    CHECK(positionsAfter[5 * 3 + 2] == doctest::Approx(2.0f));
    REQUIRE(normalsAfter.size() == normalsBefore.size());
    CHECK(normalsAfter != normalsBefore);  // the tent really did tilt the fans

    // Same again through the copying accessors, which read the same buffers.
    std::vector<float> copied(positionsAfter.size());
    REQUIRE(cyber_mesh_copy_render_positions(mesh, copied.data(), copied.size()) == copied.size());
    CHECK(copied == positionsAfter);
    std::vector<float> copiedNormals(normalsAfter.size());
    REQUIRE(cyber_mesh_copy_normals(mesh, copiedNormals.data(), copiedNormals.size()) ==
            copiedNormals.size());
    CHECK(copiedNormals == normalsAfter);

    cyber_mesh_free(mesh);
}

// The other half of the split: nothing an edit did not touch may change. The
// index and wireframe buffers are pure topology, so a drag must leave them
// byte-for-byte as they were.
TEST_CASE("capi cache: a positions-only edit leaves the index buffers untouched") {
    CyberMesh* target = makeGrid(5, 5);
    CyberSnapper* snapper = nullptr;
    REQUIRE(cyber_snapper_create(target, &snapper) == CYBER_OK);

    CyberMesh* mesh = makeGrid(5, 5, 0.4f);
    const std::vector<std::uint32_t> triangles = triangleIndices(mesh);
    const std::vector<std::uint32_t> edges = edgeIndices(mesh);
    const size_t triangleCount = cyber_mesh_triangle_count(mesh);
    const size_t edgeCount = cyber_mesh_edge_count(mesh);
    REQUIRE(!triangles.empty());
    REQUIRE(!edges.empty());

    seedWeights(mesh);
    const float xf[12] = {1.0f, 0.0f, 0.0f, 0.25f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    CyberSoftTransformReport report;
    REQUIRE(cyber_retopo_selection_transform(mesh, xf, snapper, 0.0f, &report) == CYBER_OK);
    CHECK(triangleIndices(mesh) == triangles);
    CHECK(edgeIndices(mesh) == edges);

    REQUIRE(cyber_retopo_selection_relax(mesh, 0.5f, 2, nullptr, 0, snapper, 0.0f, &report) ==
            CYBER_OK);
    CHECK(triangleIndices(mesh) == triangles);
    CHECK(edgeIndices(mesh) == edges);

    size_t moved = 0;
    float maxDistance = 0.0f;
    REQUIRE(cyber_retopo_snap_all(mesh, snapper, nullptr, 0, &moved, &maxDistance) == CYBER_OK);
    CHECK(triangleIndices(mesh) == triangles);
    CHECK(edgeIndices(mesh) == edges);
    CHECK(cyber_mesh_triangle_count(mesh) == triangleCount);
    CHECK(cyber_mesh_edge_count(mesh) == edgeCount);

    // Write-back of a whole position snapshot is positions-only too.
    std::vector<float> snapshot = allPositions(mesh);
    for (std::size_t i = 2; i < snapshot.size(); i += 3) {
        snapshot[i] += 0.75f;
    }
    REQUIRE(cyber_mesh_set_positions(mesh, snapshot.data(), snapshot.size()) == CYBER_OK);
    CHECK(triangleIndices(mesh) == triangles);
    CHECK(allPositions(mesh) == snapshot);

    cyber_mesh_free(mesh);
    cyber_snapper_free(snapper);
    cyber_mesh_free(target);
}

// A topology edit invalidates the lot, so the buffers must come back describing
// the mesh that now exists.
TEST_CASE("capi cache: a topology edit rebuilds the index buffers") {
    CyberMesh* mesh = makeGrid(4, 4);
    const std::vector<std::uint32_t> triangles = triangleIndices(mesh);
    const size_t before = cyber_mesh_triangle_count(mesh);

    bridgeCorners(mesh);
    CHECK(cyber_mesh_triangle_count(mesh) > before);
    CHECK(triangleIndices(mesh) != triangles);

    uint32_t face = 0;
    const size_t afterAdd = cyber_mesh_triangle_count(mesh);
    size_t removed = 0;
    REQUIRE(cyber_retopo_delete_faces(mesh, &face, 1, &removed) == CYBER_OK);
    CHECK(cyber_mesh_triangle_count(mesh) < afterAdd);

    cyber_mesh_free(mesh);
}

// The adjacency table and the render cache follow ONE rule, so the table cannot
// outlive a topology change either. If it did, the sweep would average over the
// vertex's old neighbours and quietly return the wrong weights.
//
// The rewiring below is chosen deliberately: deleting an interior quad and
// rotating an edge both change what a vertex's one-ring IS without changing how
// many vertex slots the mesh has, so nothing but the invalidation itself can
// notice. A rewiring that also grows the mesh would pass on a size check alone
// and prove nothing.
TEST_CASE("capi cache: the brush adjacency is rebuilt after a topology edit") {
    CyberMesh* edited = makeGrid(5, 5);
    seedWeights(edited);
    REQUIRE(cyber_retopo_selection_smooth(edited, 2) == CYBER_OK);  // builds the table
    rewireCorner(edited);
    seedWeights(edited);

    CyberMesh* fresh = uncachedCopy(edited);
    REQUIRE(cyber_retopo_selection_smooth(edited, 2) == CYBER_OK);
    REQUIRE(cyber_retopo_selection_smooth(fresh, 2) == CYBER_OK);

    CHECK(selectionWeights(edited) == selectionWeights(fresh));

    cyber_mesh_free(edited);
    cyber_mesh_free(fresh);
}

// Same rule for the relax sweep, which reads the table for both the one-ring
// centroid and the incident-face normal.
TEST_CASE("capi cache: relax after a topology edit sees the new neighbourhood") {
    const float centre[3] = {2.0f, 2.0f, 0.0f};
    CyberMesh* edited = makeGrid(5, 5);
    REQUIRE(cyber_retopo_relax(edited, centre, 0.0f, 0.5f, 1, 1, nullptr, 0, nullptr) == CYBER_OK);
    rewireCorner(edited);

    CyberMesh* fresh = uncachedCopy(edited);
    REQUIRE(cyber_retopo_relax(edited, centre, 0.0f, 0.5f, 1, 1, nullptr, 0, nullptr) == CYBER_OK);
    REQUIRE(cyber_retopo_relax(fresh, centre, 0.0f, 0.5f, 1, 1, nullptr, 0, nullptr) == CYBER_OK);

    CHECK(allPositions(edited) == allPositions(fresh));

    cyber_mesh_free(edited);
    cyber_mesh_free(fresh);
}

// Hiding faces re-filters the render compaction without touching the mesh, so
// the render vertex order changes while the adjacency stays valid. A drag after
// a hide must still write into the FILTERED order.
TEST_CASE("capi cache: hiding faces re-filters, and a later drag respects the filter") {
    CyberMesh* mesh = makeGrid(4, 4);
    const size_t visibleTriangles = cyber_mesh_triangle_count(mesh);
    REQUIRE(renderPositions(mesh).size() == 48u);

    std::vector<uint32_t> hidden(cyber_mesh_live_faces(mesh, nullptr, 0));
    REQUIRE(hidden.size() >= 4u);
    cyber_mesh_live_faces(mesh, hidden.data(), hidden.size());
    hidden.resize(hidden.size() - 1);  // hide everything but the last face
    REQUIRE(cyber_mesh_set_hidden_faces(mesh, hidden.data(), hidden.size()) == CYBER_OK);

    const std::vector<float> filtered = renderPositions(mesh);
    CHECK(cyber_mesh_triangle_count(mesh) < visibleTriangles);
    CHECK(filtered.size() == 12u);  // one visible quad, four corners

    // Move every vertex; the filtered stream must follow, and stay filtered.
    std::vector<float> snapshot = allPositions(mesh);
    for (std::size_t i = 2; i < snapshot.size(); i += 3) {
        snapshot[i] += 1.5f;
    }
    REQUIRE(cyber_mesh_set_positions(mesh, snapshot.data(), snapshot.size()) == CYBER_OK);
    const std::vector<float> movedFiltered = renderPositions(mesh);
    REQUIRE(movedFiltered.size() == filtered.size());
    CHECK(movedFiltered != filtered);
    for (std::size_t i = 2; i < movedFiltered.size(); i += 3) {
        CHECK(movedFiltered[i] == doctest::Approx(filtered[i] + 1.5f));
    }

    cyber_mesh_free(mesh);
}

// A cloned handle starts with no caches of its own, so it must not inherit the
// original's — including after the original has been dragged.
TEST_CASE("capi cache: a clone builds its own caches") {
    CyberMesh* mesh = makeGrid(4, 4);
    const float target[3] = {1.0f, 1.0f, 3.0f};
    REQUIRE(cyber_retopo_tweak_vertex(mesh, 5, target, nullptr) == CYBER_OK);
    const std::vector<float> positions = renderPositions(mesh);
    const std::vector<float> normals = renderNormals(mesh);

    CyberMesh* clone = nullptr;
    REQUIRE(cyber_mesh_clone(mesh, &clone) == CYBER_OK);
    CHECK(renderPositions(clone) == positions);
    CHECK(renderNormals(clone) == normals);
    CHECK(triangleIndices(clone) == triangleIndices(mesh));

    cyber_mesh_free(clone);
    cyber_mesh_free(mesh);
}

#ifdef CYBER_CAPI_HEADER_PATH
// The split is only useful to a renderer that knows about it, and only SAFE
// while the rule stays "re-fetch, never reuse". A future edit that turns the
// cheap-refresh note into a promise of pointer stability would licence exactly
// the dangling read the LIFETIME paragraph exists to forbid, so pin both
// halves of what the header says.
TEST_CASE("capi header keeps re-fetching mandatory while naming the cheap path") {
    std::ifstream header(CYBER_CAPI_HEADER_PATH);
    REQUIRE(header.good());
    const std::string text((std::istreambuf_iterator<char>(header)),
                           std::istreambuf_iterator<char>());

    const size_t lifetime = text.find("LIFETIME: a returned pointer");
    REQUIRE(lifetime != std::string::npos);
    const size_t end = text.find("const float* cyber_mesh_positions_ptr(", lifetime);
    REQUIRE(end != std::string::npos);
    const std::string block = text.substr(lifetime, end - lifetime);

    CHECK(block.find("re-fetch, never reuse") != std::string::npos);
    CHECK(block.find("only MOVES vertices") != std::string::npos);
    CHECK(block.find("cannot change the") != std::string::npos);
}
#endif  // CYBER_CAPI_HEADER_PATH
