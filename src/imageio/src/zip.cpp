#include "cyber/imageio/zip.hpp"

#include <cstddef>

#include "detail.hpp"

namespace cyber::imageio {

namespace {
using detail::Bytes;

void appendName(Bytes& out, const std::string& name) {
    out.insert(out.end(), name.begin(), name.end());
}
}  // namespace

void ZipWriter::add(const std::string& name, const std::vector<std::uint8_t>& data) {
    Entry e;
    e.name = name;
    e.data = data;
    e.crc = detail::crc32(data);
    entries_.push_back(std::move(e));
}

bool ZipWriter::finish(const std::string& path) {
    Bytes out;
    std::vector<std::uint32_t> localOffsets;
    localOffsets.reserve(entries_.size());

    // Local file headers + file data (STORED).
    for (const Entry& e : entries_) {
        localOffsets.push_back(static_cast<std::uint32_t>(out.size()));
        const std::uint32_t size = static_cast<std::uint32_t>(e.data.size());
        const std::uint16_t nameLen = static_cast<std::uint16_t>(e.name.size());
        detail::appendU32LE(out, 0x04034b50u);  // local file header signature
        detail::appendU16LE(out, 20);           // version needed to extract
        detail::appendU16LE(out, 0);            // general purpose flags
        detail::appendU16LE(out, 0);            // compression method: stored
        detail::appendU16LE(out, 0);            // last mod time
        detail::appendU16LE(out, 0);            // last mod date
        detail::appendU32LE(out, e.crc);        // CRC-32
        detail::appendU32LE(out, size);         // compressed size
        detail::appendU32LE(out, size);         // uncompressed size
        detail::appendU16LE(out, nameLen);      // file name length
        detail::appendU16LE(out, 0);            // extra field length
        appendName(out, e.name);
        detail::appendBytes(out, e.data);
    }

    // Central directory.
    const std::uint32_t cdStart = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        const std::uint32_t size = static_cast<std::uint32_t>(e.data.size());
        const std::uint16_t nameLen = static_cast<std::uint16_t>(e.name.size());
        detail::appendU32LE(out, 0x02014b50u);  // central directory signature
        detail::appendU16LE(out, 20);           // version made by
        detail::appendU16LE(out, 20);           // version needed to extract
        detail::appendU16LE(out, 0);            // general purpose flags
        detail::appendU16LE(out, 0);            // compression method: stored
        detail::appendU16LE(out, 0);            // last mod time
        detail::appendU16LE(out, 0);            // last mod date
        detail::appendU32LE(out, e.crc);        // CRC-32
        detail::appendU32LE(out, size);         // compressed size
        detail::appendU32LE(out, size);         // uncompressed size
        detail::appendU16LE(out, nameLen);      // file name length
        detail::appendU16LE(out, 0);            // extra field length
        detail::appendU16LE(out, 0);            // file comment length
        detail::appendU16LE(out, 0);            // disk number start
        detail::appendU16LE(out, 0);            // internal file attributes
        detail::appendU32LE(out, 0);            // external file attributes
        detail::appendU32LE(out, localOffsets[i]);  // local header offset
        appendName(out, e.name);
    }
    const std::uint32_t cdSize = static_cast<std::uint32_t>(out.size()) - cdStart;

    // End of central directory record.
    const std::uint16_t count = static_cast<std::uint16_t>(entries_.size());
    detail::appendU32LE(out, 0x06054b50u);  // EOCD signature
    detail::appendU16LE(out, 0);            // number of this disk
    detail::appendU16LE(out, 0);            // disk with central directory
    detail::appendU16LE(out, count);        // entries on this disk
    detail::appendU16LE(out, count);        // total entries
    detail::appendU32LE(out, cdSize);       // central directory size
    detail::appendU32LE(out, cdStart);      // central directory offset
    detail::appendU16LE(out, 0);            // comment length

    return detail::writeFile(path, out);
}

}  // namespace cyber::imageio
