// A sculpt handoff drives the full remesh -> unwrap -> bake pipeline
// (pipeline-bridge spec, "File handoff becomes a Target"). Split out of
// test_handoff.cpp because it needs the UV module, which is optional.
#include <doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cyber/bake/bake.hpp"
#include "cyber/core/io.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/handoff/handoff.hpp"
#include "cyber/uv/atlas.hpp"

using cyber::Index;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;
namespace handoff = cyber::handoff;
namespace io = cyber::io;
namespace fs = std::filesystem;

namespace {

// The same synthetic producer test_handoff.cpp uses: the ASCII PLY profile of
// docs/sculpt-handoff-format.md, emitting a uniformly red box.
std::string handoffBoxText() {
    const std::vector<Vec3> corners = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                       {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const std::vector<std::array<int, 4>> quads = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                                   {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    std::ostringstream out;
    out << "ply\nformat ascii 1.0\ncomment cyber_sculpt_handoff 1 0\n";
    out << "element vertex " << corners.size() << "\n";
    out << "property float x\nproperty float y\nproperty float z\n";
    out << "property float nx\nproperty float ny\nproperty float nz\n";
    out << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    out << "property float material_mix\n";
    out << "element face " << quads.size() * 2 << "\n";
    out << "property list uchar int vertex_indices\nend_header\n";
    for (const Vec3& c : corners) {
        const Vec3 n = cyber::normalized(c - Vec3{0.5f, 0.5f, 0.5f});
        out << c.x << " " << c.y << " " << c.z << " " << n.x << " " << n.y << " " << n.z
            << " 255 0 0 0.5\n";
    }
    for (const auto& q : quads) {
        out << "3 " << q[0] << " " << q[1] << " " << q[2] << "\n";
        out << "3 " << q[0] << " " << q[2] << " " << q[3] << "\n";
    }
    return out.str();
}

}  // namespace

TEST_CASE("a handoff Target remeshes, unwraps and bakes like an imported mesh") {
    const fs::path dir = fs::temp_directory_path() / "cyber_handoff_tests";
    fs::create_directories(dir);
    const fs::path path = dir / "pipeline.ply";
    std::ofstream(path, std::ios::binary) << handoffBoxText();

    const auto result = handoff::readFile(path);
    REQUIRE(result.ok());
    const Mesh& target = result.value().mesh;

    cyber::remesh::Parameters params;
    params.targetQuadCount = 200;
    const cyber::remesh::PipelineResult remeshed = cyber::remesh::remesh(target, params);
    REQUIRE(remeshed.status != cyber::remesh::RunStatus::Error);
    REQUIRE(remeshed.mesh.faceCount() > 0);

    Mesh low = remeshed.mesh;
    const cyber::uv::AtlasResult atlas = cyber::uv::unwrapAtlas(low);
    REQUIRE(atlas.chartCount > 0);
    REQUIRE(low.cornerAttributes().find<Vec2>(io::kUvAttribute) != nullptr);

    cyber::bake::BakeParams bakeParams;
    bakeParams.width = 32;
    bakeParams.height = 32;
    bakeParams.cageDistance = 0.1f;
    const cyber::bake::BakeResult baked =
        cyber::bake::bake(low, target, cyber::bake::BakeMap::Color, bakeParams);
    REQUIRE(baked.texelsCovered > 0);

    // The handoff's vertex colors survived to the bake: every covered texel is
    // the handoff's red.
    std::size_t covered = 0;
    std::size_t red = 0;
    for (int y = 0; y < baked.image.height; ++y) {
        for (int x = 0; x < baked.image.width; ++x) {
            const float r = baked.image.at(x, y, 0);
            const float g = baked.image.at(x, y, 1);
            if (r <= 0.0f && g <= 0.0f) {
                continue;  // untouched texel
            }
            ++covered;
            if (r > 0.9f && g < 0.1f) {
                ++red;
            }
        }
    }
    REQUIRE(covered > 0);
    CHECK(red == covered);
}
