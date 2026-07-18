#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal, dependency-free ZIP writer (roadmap 3.4): STORED (no compression)
// entries with correct CRC-32 and sizes. Emits local file headers, a central
// directory, and an end-of-central-directory record — the archive is readable
// by any standard ZIP tool. Used to bundle exported map images into a single
// package.
namespace cyber::imageio {

class ZipWriter {
public:
    // Adds an entry. The name is stored verbatim (use forward slashes for
    // directories); data is copied. The CRC-32 is computed here.
    void add(const std::string& name, const std::vector<std::uint8_t>& data);

    // Writes the archive to `path`. Returns false on I/O failure. Safe to call
    // with no entries (produces a valid empty archive).
    [[nodiscard]] bool finish(const std::string& path);

    [[nodiscard]] std::size_t entryCount() const { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        std::vector<std::uint8_t> data;
        std::uint32_t crc = 0;
    };
    std::vector<Entry> entries_;
};

}  // namespace cyber::imageio
