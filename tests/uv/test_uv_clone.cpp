#include <doctest.h>

#include <array>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/uv_clone.hpp"

using cyber::FaceId;
using cyber::Index;
using cyber::LoopId;
using cyber::Mesh;
using cyber::Vec2;
using cyber::Vec3;

TEST_CASE("cloneIslandUv copies UVs onto a matching island") {
    // Two disconnected quads: island A = face 0, island B = face 1.
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {2, 0, 0}, {3, 0, 0}, {3, 1, 0}, {2, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    Mesh mesh = Mesh::fromIndexed(p, f);

    auto& uv = cyber::uv::ensureUvColumn(mesh);
    const std::array<Vec2, 4> srcUvs = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    const std::vector<LoopId> srcLoops = mesh.faceLoops(FaceId{0});
    for (std::size_t k = 0; k < srcLoops.size(); ++k) {
        uv[srcLoops[k].value] = srcUvs[k];
    }

    const std::array<FaceId, 1> src = {FaceId{0}};
    const std::array<FaceId, 1> dst = {FaceId{1}};
    REQUIRE(cyber::uv::cloneIslandUv(mesh, src, dst));

    const std::vector<LoopId> dstLoops = mesh.faceLoops(FaceId{1});
    REQUIRE(dstLoops.size() == 4);
    for (std::size_t k = 0; k < dstLoops.size(); ++k) {
        REQUIRE(uv[dstLoops[k].value].x == doctest::Approx(srcUvs[k].x));
        REQUIRE(uv[dstLoops[k].value].y == doctest::Approx(srcUvs[k].y));
    }
}

TEST_CASE("cloneIslandUv rejects mismatched islands") {
    const std::vector<Vec3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {3, 0, 0},
                                 {2, 1, 0}};
    const std::vector<std::vector<Index>> f = {{0, 1, 2, 3}, {4, 5, 6}};  // quad vs triangle
    Mesh mesh = Mesh::fromIndexed(p, f);
    static_cast<void>(cyber::uv::ensureUvColumn(mesh));

    const std::array<FaceId, 1> src = {FaceId{0}};
    const std::array<FaceId, 1> dst = {FaceId{1}};
    REQUIRE_FALSE(cyber::uv::cloneIslandUv(mesh, src, dst));  // corner counts differ
}
