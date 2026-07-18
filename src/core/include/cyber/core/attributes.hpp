#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "cyber/core/math.hpp"

namespace cyber {

// Named, typed per-element attribute columns (mesh-core spec, "Generic
// element attributes"). One AttributeSet exists per element kind on a Mesh;
// the mesh keeps every column sized to its element array and applies the
// documented propagation policy from its operators:
//   - new element from interpolation (edge split, subdivision): lerp
//     (integers snap to the nearer source);
//   - merged elements: the surviving element keeps its values;
//   - copied elements (triangulation children): values copied from source.
class AttributeSet {
public:
    using Column = std::variant<std::vector<float>, std::vector<std::int32_t>, std::vector<Vec2>,
                                std::vector<Vec3>, std::vector<Vec4>>;

    template <typename T>
    std::vector<T>& create(const std::string& name) {
        auto [it, inserted] = m_columns.try_emplace(name, std::vector<T>(m_size));
        if (!inserted && !std::holds_alternative<std::vector<T>>(it->second)) {
            it->second = std::vector<T>(m_size);
        }
        return std::get<std::vector<T>>(it->second);
    }

    template <typename T>
    std::vector<T>* find(const std::string& name) {
        auto it = m_columns.find(name);
        if (it == m_columns.end()) {
            return nullptr;
        }
        return std::get_if<std::vector<T>>(&it->second);
    }

    template <typename T>
    const std::vector<T>* find(const std::string& name) const {
        auto it = m_columns.find(name);
        if (it == m_columns.end()) {
            return nullptr;
        }
        return std::get_if<std::vector<T>>(&it->second);
    }

    [[nodiscard]] bool contains(const std::string& name) const {
        return m_columns.count(name) != 0;
    }
    void remove(const std::string& name) { m_columns.erase(name); }
    [[nodiscard]] std::size_t size() const { return m_size; }
    [[nodiscard]] std::size_t columnCount() const { return m_columns.size(); }

    void resize(std::size_t n) {
        m_size = n;
        for (auto& [name, column] : m_columns) {
            std::visit([n](auto& vec) { vec.resize(n); }, column);
        }
    }

    // dst = lerp(a, b, t) in every column; integers snap to the nearer source.
    void interpolate(std::size_t dst, std::size_t a, std::size_t b, float t) {
        for (auto& [name, column] : m_columns) {
            std::visit(
                [&](auto& vec) {
                    using T = typename std::decay_t<decltype(vec)>::value_type;
                    if constexpr (std::is_same_v<T, std::int32_t>) {
                        vec[dst] = t < 0.5f ? vec[a] : vec[b];
                    } else if constexpr (std::is_same_v<T, float>) {
                        vec[dst] = vec[a] + (vec[b] - vec[a]) * t;
                    } else {
                        vec[dst] = lerp(vec[a], vec[b], t);
                    }
                },
                column);
        }
    }

    void copy(std::size_t dst, std::size_t src) {
        for (auto& [name, column] : m_columns) {
            std::visit([&](auto& vec) { vec[dst] = vec[src]; }, column);
        }
    }

    // Copies every value of every column from another set (same schema is
    // created on demand). Used when building derived meshes (subdivision).
    void adoptSchema(const AttributeSet& other) {
        for (const auto& [name, column] : other.m_columns) {
            std::visit(
                [&](const auto& vec) {
                    using T = typename std::decay_t<decltype(vec)>::value_type;
                    create<T>(name);
                },
                column);
        }
    }

    // A detached copy of one element's values across all columns; rows can
    // be captured before destructive operations and re-applied to new
    // elements (same or different AttributeSet — matched by column name).
    using RowValue = std::variant<float, std::int32_t, Vec2, Vec3, Vec4>;
    using Row = std::map<std::string, RowValue>;

    [[nodiscard]] Row extractRow(std::size_t i) const {
        Row row;
        for (const auto& [name, column] : m_columns) {
            std::visit([&](const auto& vec) { row.emplace(name, vec[i]); }, column);
        }
        return row;
    }

    void applyRow(std::size_t i, const Row& row) {
        for (const auto& [name, value] : row) {
            auto it = m_columns.find(name);
            if (it == m_columns.end()) {
                continue;
            }
            std::visit(
                [&](auto& vec) {
                    using T = typename std::decay_t<decltype(vec)>::value_type;
                    if (const T* v = std::get_if<T>(&value)) {
                        vec[i] = *v;
                    }
                },
                it->second);
        }
    }

    // Column-wise mean of several rows (integers keep the first row's value).
    [[nodiscard]] static Row averageRows(const std::vector<Row>& rows) {
        Row out;
        if (rows.empty()) {
            return out;
        }
        for (const auto& [name, first] : rows.front()) {
            RowValue acc = first;
            for (std::size_t r = 1; r < rows.size(); ++r) {
                const auto it = rows[r].find(name);
                if (it == rows[r].end()) {
                    continue;
                }
                std::visit(
                    [&](auto& a) {
                        using T = std::decay_t<decltype(a)>;
                        if constexpr (!std::is_same_v<T, std::int32_t>) {
                            if (const T* b = std::get_if<T>(&it->second)) {
                                a = a + *b;
                            }
                        }
                    },
                    acc);
            }
            const float inv = 1.0f / static_cast<float>(rows.size());
            std::visit(
                [&](auto& a) {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (!std::is_same_v<T, std::int32_t>) {
                        a = a * inv;
                    }
                },
                acc);
            out.emplace(name, acc);
        }
        return out;
    }

    template <typename Fn>
    void forEachColumnPaired(AttributeSet& other, const Fn& fn) {
        for (auto& [name, column] : m_columns) {
            auto it = other.m_columns.find(name);
            if (it == other.m_columns.end()) {
                continue;
            }
            std::visit(
                [&](auto& mine) {
                    using VecT = std::decay_t<decltype(mine)>;
                    if (auto* theirs = std::get_if<VecT>(&it->second)) {
                        fn(mine, *theirs);
                    }
                },
                column);
        }
    }

private:
    // std::map keeps column iteration order deterministic.
    std::map<std::string, Column> m_columns;
    std::size_t m_size = 0;
};

}  // namespace cyber
