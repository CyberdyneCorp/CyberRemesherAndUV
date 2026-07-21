#include "cyber/uv/packing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace cyber::uv {
namespace {

struct ShelfItem {
    std::size_t index = 0;  // original box index
    Vec2 size{};            // padded size (includes margin)
    Vec2 pos{};             // lower-left placement before normalize
};

}  // namespace

PackResult packBoxes(std::span<const Bounds2> boxes, const PackParams& params) {
    PackResult result;
    if (boxes.empty()) {
        result.ok = true;
        return result;
    }

    const float margin = std::fmax(params.margin, 0.0f);

    // Padded sizes and total area estimate.
    std::vector<ShelfItem> items;
    items.reserve(boxes.size());
    double totalArea = 0.0;
    float maxWidth = 0.0f;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        Vec2 s = boxes[i].valid() ? boxes[i].size() : Vec2{0.0f, 0.0f};
        s = {s.x + margin, s.y + margin};
        totalArea += static_cast<double>(s.x) * static_cast<double>(s.y);
        maxWidth = std::fmax(maxWidth, s.x);
        items.push_back({i, s, {}});
    }

    // Target shelf width ~ sqrt(area) gives a roughly square packing; never
    // narrower than the widest island.
    const float shelfWidth = std::fmax(maxWidth, static_cast<float>(std::sqrt(totalArea)));

    // Tallest first: both strategies waste less vertical space this way.
    std::vector<std::size_t> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return items[a].size.y > items[b].size.y; });

    float boundWidth = 0.0f;
    float boundHeight = 0.0f;
    if (params.strategy == PackStrategy::Skyline && shelfWidth > 0.0f) {
        // Bottom-left skyline over a fixed-width strip, tracked as a per-column
        // height map: each box drops into the columns whose highest point is
        // lowest, filling the vertical gaps a shelf leaves open. The best strip
        // width depends on the island shapes (too narrow forces a tall, poorly
        // normalizing stack), so several widths are tried and the one with the
        // smallest square bounding extent wins.
        constexpr int kCols = 512;
        const float baseWidth = static_cast<float>(std::sqrt(totalArea));
        const float factors[] = {0.6f, 0.75f, 0.9f, 1.0f, 1.15f, 1.35f, 1.6f, 2.0f};

        std::vector<Vec2> bestPos(items.size());
        float bestExtent = std::numeric_limits<float>::max();
        for (const float factor : factors) {
            const float stripWidth = std::fmax(maxWidth, baseWidth * factor);
            const float colWidth = stripWidth / static_cast<float>(kCols);
            std::vector<float> heights(kCols, 0.0f);
            std::vector<Vec2> pos(items.size());
            float bw = 0.0f;
            float bh = 0.0f;
            for (const std::size_t oi : order) {
                const ShelfItem& item = items[oi];
                int span = static_cast<int>(std::ceil(item.size.x / colWidth));
                span = std::clamp(span, 1, kCols);
                int bestCol = 0;
                float bestY = std::numeric_limits<float>::max();
                for (int col = 0; col + span <= kCols; ++col) {
                    float y = 0.0f;
                    for (int k = 0; k < span; ++k) {
                        y = std::fmax(y, heights[static_cast<std::size_t>(col + k)]);
                    }
                    if (y < bestY) {
                        bestY = y;
                        bestCol = col;
                    }
                }
                pos[oi] = {static_cast<float>(bestCol) * colWidth, bestY};
                const float top = bestY + item.size.y;
                for (int k = 0; k < span; ++k) {
                    heights[static_cast<std::size_t>(bestCol + k)] = top;
                }
                bw = std::fmax(bw, pos[oi].x + item.size.x);
                bh = std::fmax(bh, top);
            }
            const float extent = std::fmax(bw, bh);
            if (extent < bestExtent) {
                bestExtent = extent;
                bestPos = pos;
                boundWidth = bw;
                boundHeight = bh;
            }
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            items[i].pos = bestPos[i];
        }
    } else {
        // Tallest first: shelf packing wastes less vertical space this way.
        float cursorX = 0.0f;
        float shelfY = 0.0f;
        float shelfHeight = 0.0f;
        for (const std::size_t oi : order) {
            ShelfItem& item = items[oi];
            if (cursorX > 0.0f && cursorX + item.size.x > shelfWidth) {
                shelfY += shelfHeight;
                cursorX = 0.0f;
                shelfHeight = 0.0f;
            }
            item.pos = {cursorX, shelfY};
            cursorX += item.size.x;
            shelfHeight = std::fmax(shelfHeight, item.size.y);
            boundWidth = std::fmax(boundWidth, cursorX);
            boundHeight = std::fmax(boundHeight, shelfY + shelfHeight);
        }
    }

    const float extent = std::fmax(boundWidth, boundHeight);
    const float scale = extent > 0.0f ? 1.0f / extent : 1.0f;

    result.islands.resize(boxes.size());
    double placedArea = 0.0;
    for (const ShelfItem& item : items) {
        PackedIsland packed;
        packed.source = boxes[item.index];
        packed.scale = scale;
        // Padded box lower-left is item.pos; the actual island sits inside the
        // padding, offset by half the margin.
        const float half = margin * 0.5f;
        packed.offset = {(item.pos.x + half) * scale, (item.pos.y + half) * scale};
        const Vec2 islandSize =
            packed.source.valid() ? packed.source.size() : Vec2{0.0f, 0.0f};
        packed.placed.mn = packed.offset;
        packed.placed.mx = {packed.offset.x + islandSize.x * scale,
                            packed.offset.y + islandSize.y * scale};
        placedArea += static_cast<double>(packed.placed.area());
        result.islands[item.index] = packed;
    }

    result.ok = true;
    result.scale = scale;
    result.usedArea = static_cast<float>(placedArea);
    result.texelDensity = static_cast<float>(params.textureSize) * scale;
    return result;
}

PackResult packIslands(Mesh& mesh, std::span<const std::vector<FaceId>> islands,
                       const PackParams& params) {
    std::vector<Bounds2> boxes;
    boxes.reserve(islands.size());
    for (const std::vector<FaceId>& island : islands) {
        boxes.push_back(islandUvBounds(mesh, island));
    }

    const PackResult result = packBoxes(boxes, params);
    if (!result.ok) {
        return result;
    }

    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return result;
    }
    for (std::size_t i = 0; i < islands.size(); ++i) {
        const PackedIsland& packed = result.islands[i];
        const Vec2 srcMin = packed.source.valid() ? packed.source.mn : Vec2{0.0f, 0.0f};
        for (const FaceId face : islands[i]) {
            for (const LoopId loop : mesh.faceLoops(face)) {
                Vec2& p = (*uv)[static_cast<std::size_t>(loop.value)];
                p = {(p.x - srcMin.x) * packed.scale + packed.offset.x,
                     (p.y - srcMin.y) * packed.scale + packed.offset.y};
            }
        }
    }
    return result;
}

float texelDensity(const Mesh& mesh, std::span<const FaceId> island, int textureSize) {
    const std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return 0.0f;
    }
    double ratioSum = 0.0;
    std::size_t count = 0;
    for (const FaceId face : island) {
        const std::vector<LoopId> loops = mesh.faceLoops(face);
        const std::size_t n = loops.size();
        for (std::size_t i = 0; i < n; ++i) {
            const LoopId a = loops[i];
            const LoopId b = loops[(i + 1) % n];
            const float worldLen =
                length(mesh.position(mesh.loopVertex(b)) - mesh.position(mesh.loopVertex(a)));
            if (worldLen <= 0.0f) {
                continue;
            }
            const Vec2 uvEdge =
                (*uv)[static_cast<std::size_t>(b.value)] - (*uv)[static_cast<std::size_t>(a.value)];
            const float uvLen = std::sqrt(uvEdge.x * uvEdge.x + uvEdge.y * uvEdge.y);
            ratioSum += static_cast<double>(uvLen) / static_cast<double>(worldLen);
            ++count;
        }
    }
    if (count == 0) {
        return 0.0f;
    }
    return static_cast<float>(ratioSum / static_cast<double>(count)) *
           static_cast<float>(textureSize);
}

}  // namespace cyber::uv
