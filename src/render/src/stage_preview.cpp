#include "cyber/render/stage_preview.hpp"

#include <algorithm>
#include <cmath>

namespace cyber::render {

const char* toString(PreviewStage stage) {
    switch (stage) {
        case PreviewStage::Source:
            return "Source";
        case PreviewStage::Isotropic:
            return "Isotropic";
        case PreviewStage::Param:
            return "Param";
        case PreviewStage::Result:
            return "Result";
    }
    return "Unknown";
}

Vec4 singularityColor(std::int32_t valence) {
    if (valence == 4) {
        return {0.6f, 0.6f, 0.6f, 1.0f};  // regular — neutral grey
    }
    // Intensity scales with how far the valence departs from regular, saturating
    // at a difference of 3 (valence 1 or 7).
    const float delta = static_cast<float>(std::abs(valence - 4));
    const float intensity = std::min(delta / 3.0f, 1.0f);
    if (valence < 4) {
        return {0.2f, 0.4f + 0.5f * intensity, 1.0f, 1.0f};  // blue: low valence
    }
    return {1.0f, 0.4f - 0.3f * intensity, 0.2f, 1.0f};  // red: high valence
}

std::vector<std::pair<Vec3, Vec3>> crossFieldSegments(const std::vector<CrossFieldSample>& samples,
                                                      float length) {
    std::vector<std::pair<Vec3, Vec3>> segments;
    segments.reserve(samples.size() * 2u);
    for (const CrossFieldSample& s : samples) {
        const Vec3 d0 = normalized(s.dir0) * length;
        const Vec3 d1 = normalized(s.dir1) * length;
        segments.emplace_back(s.position - d0, s.position + d0);
        segments.emplace_back(s.position - d1, s.position + d1);
    }
    return segments;
}

std::vector<std::pair<Vec2, Vec2>> crossPatternSegments(const CrossPatternPreview& pattern) {
    std::vector<std::pair<Vec2, Vec2>> segments;
    const std::uint32_t linesU = std::max<std::uint32_t>(pattern.linesU, 2);
    const std::uint32_t linesV = std::max<std::uint32_t>(pattern.linesV, 2);
    segments.reserve(linesU + linesV);

    const float uSpan = pattern.uvMax.x - pattern.uvMin.x;
    const float vSpan = pattern.uvMax.y - pattern.uvMin.y;

    // Iso-lines of constant u (vertical in UV space).
    for (std::uint32_t i = 0; i < linesU; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(linesU - 1);
        const float u = pattern.uvMin.x + uSpan * t;
        segments.emplace_back(Vec2{u, pattern.uvMin.y}, Vec2{u, pattern.uvMax.y});
    }
    // Iso-lines of constant v (horizontal in UV space).
    for (std::uint32_t i = 0; i < linesV; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(linesV - 1);
        const float v = pattern.uvMin.y + vSpan * t;
        segments.emplace_back(Vec2{pattern.uvMin.x, v}, Vec2{pattern.uvMax.x, v});
    }
    return segments;
}

StagePreviewDescriptor defaultDescriptor(PreviewStage stage) {
    StagePreviewDescriptor d;
    d.stage = stage;
    switch (stage) {
        case PreviewStage::Source:
            d.showWireframe = true;
            d.tint = {0.85f, 0.85f, 0.9f, 1.0f};
            break;
        case PreviewStage::Isotropic:
            d.showWireframe = true;
            d.tint = {0.8f, 0.9f, 0.85f, 1.0f};
            break;
        case PreviewStage::Param:
            d.showWireframe = false;
            d.showSingularities = true;
            d.showCrossField = true;
            d.showUvGrid = true;
            d.tint = {0.9f, 0.85f, 0.8f, 1.0f};
            break;
        case PreviewStage::Result:
            d.showWireframe = true;
            d.showSingularities = true;
            d.showUvGrid = false;
            d.tint = {1.0f, 1.0f, 1.0f, 1.0f};
            break;
    }
    return d;
}

}  // namespace cyber::render
