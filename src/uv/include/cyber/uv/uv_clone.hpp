#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

namespace cyber::uv {

// Copies per-corner UVs from a source island onto a topologically-matching
// destination island (same face count and matching per-face corner counts, in
// island order). Returns false if the islands do not match or the mesh has no
// UVs. This is the data operation behind "UV clone between matching islands"
// (uv-editing 10.3); the on-model multitouch gesture + tileable preview that
// drive it are viewport-side (group 8).
[[nodiscard]] inline bool cloneIslandUv(Mesh& mesh, std::span<const FaceId> src,
                                        std::span<const FaceId> dst) {
    if (src.size() != dst.size()) {
        return false;
    }
    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return false;
    }
    // Validate matching corner counts first, so a mismatch leaves UVs untouched.
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (mesh.faceLoops(src[i]).size() != mesh.faceLoops(dst[i]).size()) {
            return false;
        }
    }
    for (std::size_t i = 0; i < src.size(); ++i) {
        const std::vector<LoopId> sl = mesh.faceLoops(src[i]);
        const std::vector<LoopId> dl = mesh.faceLoops(dst[i]);
        for (std::size_t k = 0; k < sl.size(); ++k) {
            (*uv)[dl[k].value] = (*uv)[sl[k].value];
        }
    }
    return true;
}

}  // namespace cyber::uv
