#include "cyber/uv/uv_sets.hpp"

#include <algorithm>
#include <cstring>

namespace cyber::uv {
namespace {

[[nodiscard]] std::string storedName(const std::string& name) {
    return std::string(kUvSetPrefix) + name;
}

// A set name must be non-empty and must not contain the ':' separator, or a stored column's name
// would be ambiguous with a differently-split pair.
[[nodiscard]] bool isValidName(const std::string& name) {
    return !name.empty() && name.find(':') == std::string::npos;
}

constexpr std::uint32_t kMagic = 0x55565354;  // "UVST"
constexpr std::uint32_t kVersion = 1;

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    // Explicit little-endian rather than memcpy of the native representation: a document written
    // on one architecture must load on another.
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

[[nodiscard]] bool readU32(const std::vector<std::uint8_t>& in, std::size_t& at,
                           std::uint32_t& out) {
    if (at + 4 > in.size()) {
        return false;
    }
    out = static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1]) << 8) |
          (static_cast<std::uint32_t>(in[at + 2]) << 16) |
          (static_cast<std::uint32_t>(in[at + 3]) << 24);
    at += 4;
    return true;
}

void appendFloat(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(out, bits);
}

[[nodiscard]] bool readFloat(const std::vector<std::uint8_t>& in, std::size_t& at, float& out) {
    std::uint32_t bits = 0;
    if (!readU32(in, at, bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

void appendString(std::vector<std::uint8_t>& out, const std::string& value) {
    appendU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

[[nodiscard]] bool readString(const std::vector<std::uint8_t>& in, std::size_t& at,
                             std::string& out) {
    std::uint32_t length = 0;
    if (!readU32(in, at, length) || at + length > in.size()) {
        return false;
    }
    out.assign(in.begin() + static_cast<std::ptrdiff_t>(at),
               in.begin() + static_cast<std::ptrdiff_t>(at + length));
    at += length;
    return true;
}

}  // namespace

std::vector<std::string> uvSetNames(const Mesh& mesh, const std::string& activeName) {
    std::vector<std::string> out;
    if (uvColumn(mesh) != nullptr) {
        out.push_back(activeName.empty() ? std::string(kDefaultUvSetName) : activeName);
    }
    for (const std::string& column : mesh.cornerAttributes().names()) {
        if (column.rfind(kUvSetPrefix, 0) == 0) {
            out.push_back(column.substr(kUvSetPrefix.size()));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool hasUvSet(const Mesh& mesh, const std::string& activeName, const std::string& name) {
    const std::vector<std::string> names = uvSetNames(mesh, activeName);
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool createUvSet(Mesh& mesh, const std::string& activeName, const std::string& name) {
    if (!isValidName(name) || hasUvSet(mesh, activeName, name)) {
        return false;
    }
    const std::vector<Vec2>* active = uvColumn(mesh);
    if (active == nullptr) {
        // Nothing to copy. Creating an empty set would produce a full column of (0,0) that reads
        // downstream as a real layout collapsed at the origin.
        return false;
    }
    const std::vector<Vec2> copy = *active;
    mesh.cornerAttributes().create<Vec2>(storedName(name)) = copy;
    return true;
}

bool activateUvSet(Mesh& mesh, const std::string& activeName, const std::string& name) {
    if (name == activeName || !isValidName(name)) {
        return false;
    }
    std::vector<Vec2>* incoming = mesh.cornerAttributes().find<Vec2>(storedName(name));
    if (incoming == nullptr) {
        return false;
    }
    // Copied out BEFORE touching anything: `create` below can invalidate `incoming` by rehashing
    // the column map, and reading a dangling pointer afterwards would write garbage UVs.
    const std::vector<Vec2> promoted = *incoming;

    if (const std::vector<Vec2>* outgoing = uvColumn(mesh); outgoing != nullptr) {
        const std::vector<Vec2> demoted = *outgoing;
        const std::string keepAs = activeName.empty() ? std::string(kDefaultUvSetName) : activeName;
        mesh.cornerAttributes().create<Vec2>(storedName(keepAs)) = demoted;
    }
    ensureUvColumn(mesh) = promoted;
    mesh.cornerAttributes().remove(storedName(name));
    return true;
}

bool deleteUvSet(Mesh& mesh, const std::string& activeName, const std::string& name) {
    if (name == activeName || !isValidName(name)) {
        // Refusing the active set is deliberate: deleting it would leave the `uv` column belonging
        // to a set the caller still names, or no active set at all.
        return false;
    }
    if (mesh.cornerAttributes().find<Vec2>(storedName(name)) == nullptr) {
        return false;
    }
    mesh.cornerAttributes().remove(storedName(name));
    return true;
}

bool renameUvSet(Mesh& mesh, const std::string& activeName, const std::string& from,
                 const std::string& to) {
    if (!isValidName(to) || from == to || hasUvSet(mesh, activeName, to)) {
        return false;
    }
    if (from == activeName) {
        // The active set's name lives with the CALLER, so renaming it moves no column at all. The
        // caller records the new name; reporting success here is what tells it to.
        return true;
    }
    std::vector<Vec2>* column = mesh.cornerAttributes().find<Vec2>(storedName(from));
    if (column == nullptr) {
        return false;
    }
    const std::vector<Vec2> moved = *column;
    mesh.cornerAttributes().create<Vec2>(storedName(to)) = moved;
    mesh.cornerAttributes().remove(storedName(from));
    return true;
}

std::vector<std::uint8_t> serializeUvSets(const Mesh& mesh, const std::string& activeName) {
    std::vector<std::uint8_t> out;
    const std::vector<std::string> names = uvSetNames(mesh, activeName);
    const std::string active = activeName.empty() ? std::string(kDefaultUvSetName) : activeName;

    appendU32(out, kMagic);
    appendU32(out, kVersion);
    // The corner count, so a sidecar written for different topology is REJECTED rather than
    // silently misapplied — a UV set on the wrong topology shears every island, which looks
    // plausible and is wrong.
    appendU32(out, static_cast<std::uint32_t>(mesh.cornerAttributes().size()));
    appendString(out, active);
    appendU32(out, static_cast<std::uint32_t>(names.size()));

    for (const std::string& name : names) {
        appendString(out, name);
        // The ACTIVE set's data is written as zero-length: the payload owns it. Storing it here
        // too would let a stale copy overwrite a newer layout on load.
        const std::vector<Vec2>* column =
            name == active ? nullptr : mesh.cornerAttributes().find<Vec2>(storedName(name));
        const std::uint32_t count =
            column == nullptr ? 0 : static_cast<std::uint32_t>(column->size());
        appendU32(out, count);
        if (column != nullptr) {
            for (const Vec2& uv : *column) {
                appendFloat(out, uv.x);
                appendFloat(out, uv.y);
            }
        }
    }
    return out;
}

std::string deserializeUvSets(Mesh& mesh, const std::vector<std::uint8_t>& bytes) {
    std::size_t at = 0;
    std::uint32_t magic = 0, version = 0, corners = 0, setCount = 0;
    std::string active;
    if (!readU32(bytes, at, magic) || magic != kMagic) {
        return {};
    }
    if (!readU32(bytes, at, version) || version != kVersion) {
        return {};
    }
    if (!readU32(bytes, at, corners) ||
        corners != static_cast<std::uint32_t>(mesh.cornerAttributes().size())) {
        return {};
    }
    if (!readString(bytes, at, active) || !readU32(bytes, at, setCount)) {
        return {};
    }

    // Parsed WHOLE into a staging list before anything is written, so a truncated blob leaves the
    // mesh exactly as it was rather than half-restored.
    std::vector<std::pair<std::string, std::vector<Vec2>>> parsed;
    parsed.reserve(setCount);
    for (std::uint32_t i = 0; i < setCount; ++i) {
        std::string name;
        std::uint32_t count = 0;
        if (!readString(bytes, at, name) || !readU32(bytes, at, count)) {
            return {};
        }
        if (count != 0 && count != corners) {
            return {};
        }
        std::vector<Vec2> column(count);
        for (std::uint32_t k = 0; k < count; ++k) {
            if (!readFloat(bytes, at, column[k].x) || !readFloat(bytes, at, column[k].y)) {
                return {};
            }
        }
        parsed.emplace_back(std::move(name), std::move(column));
    }
    if (active.empty()) {
        return {};
    }

    // Drop existing stored sets so a restore REPLACES rather than merges: a merge would leave sets
    // from whatever was loaded before, which no save described.
    for (const std::string& column : mesh.cornerAttributes().names()) {
        if (column.rfind(kUvSetPrefix, 0) == 0) {
            mesh.cornerAttributes().remove(column);
        }
    }
    for (auto& [name, column] : parsed) {
        // The active set is skipped on BOTH counts: it is written empty, and even a non-empty one
        // must not overwrite `uv`, which the payload owns.
        if (column.empty() || name == active) {
            continue;
        }
        mesh.cornerAttributes().create<Vec2>(storedName(name)) = column;
    }
    return active;
}

}  // namespace cyber::uv
