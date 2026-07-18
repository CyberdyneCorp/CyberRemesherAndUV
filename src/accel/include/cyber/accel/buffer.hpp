#pragma once

#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

namespace cyber::accel {

// Typed device buffer (compute-acceleration spec, "typed device buffers").
//
// The CPU backend — the mandatory reference — backs a Buffer with host memory,
// so `data()`/`operator[]` address it directly. GPU backends (tasks 4.3-4.5)
// will provide device-resident storage behind the same handle and stage host
// copies through `upload`/`download`; on the CPU those are trivial. Keeping
// the primitive API in terms of Buffer<T> (never raw std::vector) lets those
// backends slot in without touching call sites.
template <class T>
class Buffer {
public:
    Buffer() = default;
    explicit Buffer(std::size_t count) : m_data(count) {}
    Buffer(std::initializer_list<T> values) : m_data(values) {}
    explicit Buffer(std::vector<T> values) : m_data(std::move(values)) {}

    [[nodiscard]] std::size_t size() const { return m_data.size(); }
    [[nodiscard]] bool empty() const { return m_data.empty(); }
    void resize(std::size_t count) { m_data.resize(count); }

    [[nodiscard]] T* data() { return m_data.data(); }
    [[nodiscard]] const T* data() const { return m_data.data(); }
    [[nodiscard]] T& operator[](std::size_t i) { return m_data[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const { return m_data[i]; }

    // Host mirror of the buffer contents. On the CPU backend this is the
    // storage itself; on GPU backends it forces a download.
    [[nodiscard]] const std::vector<T>& host() const { return m_data; }

    // Replace the contents from host memory (host -> device on GPU backends).
    void upload(const std::vector<T>& values) { m_data = values; }
    // Copy the contents to host memory (device -> host on GPU backends).
    void download(std::vector<T>& out) const { out = m_data; }

private:
    std::vector<T> m_data;
};

}  // namespace cyber::accel
