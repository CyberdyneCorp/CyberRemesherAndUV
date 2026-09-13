#include <happly.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "io_internal.hpp"

namespace cyber::io::detail {

namespace {

// happly sizes every element buffer from the header's DECLARED count before it
// reads a single datum, so a 40-byte header claiming 37 777 777 777 vertices
// makes it ask the allocator for ~151 GB. The mesh-IO fuzzer found exactly that
// shape in an 823-byte file, and the fuzz target's contract names it: a
// malformed file must never become "an allocation sized from an
// attacker-controlled count".
//
// Binary STL has always refused this by checking its declared triangle count
// against the file size (`fileSize != 84 + count * 50`). PLY gets the same
// guard, in our code rather than as a patch to the vendored library.
//
// What is computed is a LOWER bound on the bytes an element instance must
// occupy, so the check can only reject a file already short of its own header's
// claim -- a legitimate file cannot trip it, because every property costs at
// least this much however it is encoded.

// Bytes a binary property of this type occupies; 0 for a type name we do not
// recognise, which leaves the element unbounded and so not ours to judge.
std::uintmax_t plyBinaryTypeSize(const std::string& type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") {
        return 1;
    }
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") {
        return 2;
    }
    if (type == "int" || type == "uint" || type == "int32" || type == "uint32" || type == "float" ||
        type == "float32") {
        return 4;
    }
    if (type == "double" || type == "float64") {
        return 8;
    }
    return 0;
}

// In ASCII a property is at minimum one digit plus its separator. With no
// readable `format` line we cannot tell ASCII from binary, and the smallest a
// binary property can be is one byte, so that is the universal floor.
constexpr std::uintmax_t kMinAsciiProperty = 2;
constexpr std::uintmax_t kMinUnknownProperty = 1;

// happly matches header keywords with a PREFIX test, not by token equality, so
// `end_headerelefent face 1` ends the header for it. Mirroring that exactly is
// half of not diverging from the parser we are guarding; the other half is the
// fileSize fallback below, which keeps a divergence harmless rather than fatal.
bool startsWith(const std::string& line, std::string_view prefix) {
    return line.rfind(prefix, 0) == 0;
}
// A header is small, and refusing to scan forever is itself part of the guard.
constexpr std::uintmax_t kMaxHeaderBytes = 1u << 20;

struct PlyElement {
    std::uintmax_t count = 0;
    std::uintmax_t minBytes = 0;
    // Cleared when this element declares a property we cannot size. Per-element
    // and NOT global: a first version made one unreadable line abandon the
    // whole check, and the fuzzer immediately produced a header whose first
    // element was mutated and whose THIRD still declared 37 777 777 777.
    // Giving up on the file because one line is unreadable hands the rest of
    // it a free pass.
    bool bounded = true;
};

// happly reads the count as `std::istringstream iss(token); iss >> count;`
// into a size_t, which takes the leading digits and stops at the first
// character that is not one. Mirror that EXACTLY rather than validating more
// strictly: every version of this guard that parsed more carefully than the
// parser it guards ended up rejecting a count happly accepted, skipping the
// element, and letting the allocation through. A count of "3777777777\0...7"
// is 3 777 777 777 to happly, and it has to be that here too.
//
// On failure `>>` stores 0 (C++11), and on overflow it stores the maximum --
// both of which are then what happly itself will act on, so agreeing with it is
// the whole point.
bool parseElementCount(const std::string& text, std::uintmax_t& out) {
    std::istringstream iss(text);
    std::size_t value = 0;
    iss >> value;
    out = value;
    return true;
}

// The minimum bytes one instance of the element being declared costs, added to
// the element the property line belongs to.
void accumulateProperty(std::istringstream& words, bool ascii, bool sawFormat,
                        PlyElement& element) {
    std::string type;
    words >> type;
    if (type == "list") {
        // A list contributes at least its count field; the entries it announces
        // are bounded by the same file-size argument once they are read.
        words >> type;
    }
    std::uintmax_t size = kMinUnknownProperty;
    if (sawFormat) {
        size = ascii ? kMinAsciiProperty : plyBinaryTypeSize(type);
    }
    if (size == 0) {
        element.bounded = false;
        return;
    }
    element.minBytes += size;
}

// Declared element counts against the bytes that actually follow the header.
// Returns the refusal when the file cannot possibly hold what it declares, and
// nullopt whenever the header is anything we do not fully understand -- this is
// a guard in front of happly, not a replacement for its parser.
std::optional<std::string> plyHeaderExceedsFile(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    std::ifstream file(path, std::ios::binary);
    if (ec || !file) {
        return std::nullopt;
    }

    bool ascii = false;
    bool sawFormat = false;
    bool ended = false;
    std::vector<PlyElement> elements;
    std::uintmax_t headerBytes = 0;
    std::string line;

    while (headerBytes <= kMaxHeaderBytes && std::getline(file, line)) {
        // +1 for the delimiter getline consumed -- except on a final line that
        // has none, where it overshoots. That matters only because the running
        // total is compared against the file size below, and a header with no
        // end_header is read to EOF, so the overshoot is exactly the case the
        // fallback exists for. Clamped rather than conditionalised: this is a
        // lower-bound argument, and a byte of slack cannot weaken it.
        headerBytes = std::min(headerBytes + line.size() + 1, fileSize);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream words(line);
        std::string keyword;
        words >> keyword;

        if (startsWith(line, "format")) {
            std::string encoding;
            words >> encoding;
            ascii = encoding == "ascii";
            sawFormat = true;
        } else if (startsWith(line, "element")) {
            std::string name;
            std::string count;
            words >> name >> count;
            PlyElement element;
            parseElementCount(count, element.count);
            elements.push_back(element);
        } else if (startsWith(line, "property") && !elements.empty()) {
            accumulateProperty(words, ascii, sawFormat, elements.back());
        } else if (startsWith(line, "end_header")) {
            ended = true;
            break;
        }
    }

    // Not finding `end_header` used to abandon the check, and that is exactly
    // how the second fuzz round got through: happly's prefix match accepted
    // `end_headerelefent face 1` as the end of the header, this token-equality
    // test did not, so the guard deferred and happly allocated 226 GB.
    //
    // The lesson generalises past that one line. ANY disagreement with the
    // parser about where the header stops must stay harmless, so when the end
    // is unknown the bound falls back to the WHOLE FILE. No legitimate file can
    // contain an element needing more bytes than the file itself, whatever the
    // header turns out to say, so this cannot reject valid input -- it is
    // simply a weaker bound than the precise one.
    const std::uintmax_t payload = ended ? fileSize - headerBytes : fileSize;
    std::uintmax_t needed = 0;
    for (const PlyElement& element : elements) {
        if (!element.bounded || element.minBytes == 0 || element.count == 0) {
            continue;
        }
        // Ordered to compare rather than multiply: the product is what overflows.
        if (element.count > (payload - needed) / element.minBytes) {
            return "declares " + std::to_string(element.count) + " element(s) needing at least " +
                   std::to_string(element.count) + " x " + std::to_string(element.minBytes) +
                   " bytes, but only " + std::to_string(payload) + " follow the header";
        }
        needed += element.count * element.minBytes;
    }
    return std::nullopt;
}

std::optional<std::uintmax_t> declaredPlyVertexCount(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::uintmax_t headerBytes = 0;
    std::string line;
    while (headerBytes <= kMaxHeaderBytes && std::getline(file, line)) {
        headerBytes += line.size() + 1;
        if (startsWith(line, "element")) {
            std::istringstream words(line);
            std::string keyword;
            std::string name;
            std::string count;
            words >> keyword >> name >> count;
            if (name == "vertex") {
                std::uintmax_t value = 0;
                parseElementCount(count, value);
                return value;
            }
        }
        if (startsWith(line, "end_header")) {
            break;
        }
    }
    return std::nullopt;
}

}  // namespace

Result<ImportedMesh> importPly(const std::filesystem::path& path, const ImportOptions& options) {
    if (const std::optional<std::string> refusal = plyHeaderExceedsFile(path)) {
        return Error{ErrorCode::ParseError, "'" + path.string() + "' " + *refusal};
    }
    if (options.maxVertices > 0) {
        const std::optional<std::uintmax_t> declared = declaredPlyVertexCount(path);
        if (declared && *declared > options.maxVertices) {
            return Error{ErrorCode::ResourceLimit,
                         "'" + path.string() + "' declares " + std::to_string(*declared) +
                             " vertices, over this host's vertex ceiling of " +
                             std::to_string(options.maxVertices)};
        }
    }
    try {
        happly::PLYData ply(path.string());
        ply.validate();

        const std::vector<std::array<double, 3>> positions = ply.getVertexPositions();
        std::vector<std::vector<std::size_t>> faces = ply.getFaceIndices<std::size_t>();

        ImportedMesh out;
        std::vector<VertexId> ids;
        ids.reserve(positions.size());
        for (const auto& p : positions) {
            ids.push_back(out.mesh.addVertex(
                {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])}));
        }

        if (ply.getElement("vertex").hasProperty("red")) {
            const std::vector<std::array<unsigned char, 3>> colors = ply.getVertexColors();
            auto& column = out.mesh.vertexAttributes().create<Vec3>(kColorAttribute);
            for (std::size_t i = 0; i < colors.size() && i < ids.size(); ++i) {
                column[ids[i].value] = {static_cast<float>(colors[i][0]) / 255.0f,
                                        static_cast<float>(colors[i][1]) / 255.0f,
                                        static_cast<float>(colors[i][2]) / 255.0f};
            }
        }

        std::size_t skipped = 0;
        std::vector<VertexId> faceVerts;
        for (const auto& face : faces) {
            faceVerts.clear();
            bool ok = face.size() >= 3;
            for (const std::size_t i : face) {
                if (i >= ids.size()) {
                    ok = false;
                    break;
                }
                faceVerts.push_back(ids[i]);
            }
            const FaceId f = ok ? out.mesh.addFace(faceVerts) : FaceId{};
            if (!f.valid()) {
                ++skipped;
                continue;
            }
            if (options.polygons == PolygonPolicy::Triangulate && faceVerts.size() > 3) {
                out.mesh.triangulateFace(f);
            }
        }

        if (out.mesh.faceCount() == 0) {
            return Error{ErrorCode::EmptyMesh, "no usable faces in '" + path.string() + "'"};
        }
        if (skipped > 0) {
            out.warnings.push_back("skipped " + std::to_string(skipped) + " degenerate face(s)");
        }
        computeBounds(out);
        return out;
    } catch (const std::exception& e) {
        return Error{ErrorCode::ParseError, "failed to parse '" + path.string() + "': " + e.what()};
    }
}

Status exportPly(const Mesh& mesh, const std::filesystem::path& path,
                 const ExportOptions& /*options*/) {
    try {
        std::vector<Vec3> positions;
        std::vector<std::vector<Index>> faces;
        mesh.toIndexed(positions, faces);

        std::vector<std::array<double, 3>> plyPositions;
        plyPositions.reserve(positions.size());
        for (const Vec3& p : positions) {
            plyPositions.push_back({p.x, p.y, p.z});
        }
        std::vector<std::vector<std::size_t>> plyFaces;
        plyFaces.reserve(faces.size());
        for (const auto& f : faces) {
            plyFaces.emplace_back(f.begin(), f.end());
        }

        happly::PLYData ply;
        ply.addVertexPositions(plyPositions);
        ply.addFaceIndices(plyFaces);

        if (const auto* colors = mesh.vertexAttributes().find<Vec3>(kColorAttribute)) {
            std::vector<std::array<unsigned char, 3>> plyColors;
            plyColors.reserve(positions.size());
            for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
                if (!mesh.isAlive(VertexId{i})) {
                    continue;
                }
                const Vec3 c = (*colors)[i];
                auto clamp255 = [](float v) {
                    return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                };
                plyColors.push_back({{clamp255(c.x), clamp255(c.y), clamp255(c.z)}});
            }
            ply.addVertexColors(plyColors);
        }

        ply.write(path.string(), happly::DataFormat::Binary);
        return {};
    } catch (const std::exception& e) {
        return Error{ErrorCode::WriteFailed,
                     "write to '" + path.string() + "' failed: " + e.what()};
    }
}

}  // namespace cyber::io::detail
