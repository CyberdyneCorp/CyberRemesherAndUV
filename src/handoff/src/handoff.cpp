#include "cyber/handoff/handoff.hpp"

#include <happly.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
#include <istream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace cyber::handoff {

namespace {

using io::Error;
using io::ErrorCode;
using io::Result;

constexpr const char* kVersionComment = "cyber_sculpt_handoff";
constexpr const char* kProducerComment = "cyber_handoff_producer";

std::string versionText(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor);
}

std::string supportedText() {
    return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor);
}

// The compatibility rule from docs/sculpt-handoff-format.md: same major, minor
// no newer than ours. Everything else is a typed rejection naming both sides.
Error versionRejection(std::string_view origin, const Version& found) {
    return Error{ErrorCode::IncompatibleVersion,
                 std::string(origin) + ": sculpt handoff version " + versionText(found) +
                     " is not supported (this engine supports " + std::to_string(kVersionMajor) +
                     ".x up to " + supportedText() + ")"};
}

bool versionSupported(const Version& v) {
    return v.major == kVersionMajor && v.minor <= kVersionMinor && v.minor >= 0;
}

// Splits a comment line into whitespace-separated tokens. PLY comments arrive
// from happly with the leading "comment " already stripped.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::optional<int> parseInt(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return std::nullopt;
        }
    }
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// What the PLY header declared, read WITHOUT touching any geometry.
struct HeaderDeclaration {
    bool found = false;
    bool malformed = false;
    Version version;
    std::string producer;
};

HeaderDeclaration scanComments(const std::vector<std::string>& comments) {
    HeaderDeclaration out;
    for (const std::string& comment : comments) {
        const std::vector<std::string> tokens = tokenize(comment);
        if (tokens.empty()) {
            continue;
        }
        if (tokens[0] == kVersionComment) {
            out.found = true;
            const std::optional<int> major = tokens.size() > 1 ? parseInt(tokens[1]) : std::nullopt;
            const std::optional<int> minor = tokens.size() > 2 ? parseInt(tokens[2]) : std::nullopt;
            if (!major || !minor) {
                out.malformed = true;
                continue;
            }
            out.version = Version{*major, *minor};
        } else if (tokens[0] == kProducerComment && tokens.size() > 1) {
            // Take everything after the key, not the first occurrence of the
            // first token: searching the whole line finds the label inside the
            // key itself whenever the label is a substring of it (producer "a"
            // matched the "a" in "cyber_handoff_producer", yielding
            // "andoff_producer a"). The label may contain spaces, so it runs to
            // the end of the comment.
            const std::size_t after =
                comment.find(kProducerComment) + std::strlen(kProducerComment);
            const std::size_t start = comment.find_first_not_of(" \t", after);
            out.producer = start == std::string::npos ? std::string() : comment.substr(start);
        }
    }
    return out;
}

// The PLY header as raw text, up to and including "end_header". Scanned before
// happly parses anything so a bad version can never build a partial Target.
std::vector<std::string> headerComments(std::string_view bytes) {
    std::vector<std::string> comments;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::size_t eol = bytes.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = bytes.size();
        }
        std::string_view line = bytes.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = eol + 1;
        if (line.rfind("comment", 0) == 0) {
            std::string_view rest = line.substr(7);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                rest.remove_prefix(1);
            }
            comments.emplace_back(rest);
        }
        if (line == "end_header") {
            break;
        }
    }
    return comments;
}

void computeBounds(Handoff& out) {
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (Index i = 0; i < out.mesh.vertexCapacity(); ++i) {
        if (!out.mesh.isAlive(VertexId{i})) {
            continue;
        }
        const Vec3 p = out.mesh.position(VertexId{i});
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        any = true;
    }
    out.boundsMin = any ? lo : Vec3{};
    out.boundsMax = any ? hi : Vec3{};
}

// A face the Mesh refused (a repeated vertex index — the routine degenerate in
// a sculpt export) is geometry the producer described and the Target does not
// have. The pipeline-bridge spec's loudness requirement makes that a counted,
// warned-about loss rather than a silently smaller mesh.
void reportDroppedFaces(Handoff& out, std::string_view origin, std::size_t described) {
    if (out.droppedFaces == 0) {
        return;
    }
    out.warnings.emplace_back(std::string(origin) + ": dropped " +
                              std::to_string(out.droppedFaces) + " of " +
                              std::to_string(described) +
                              " handoff triangle(s) the mesh refused (a repeated vertex index)");
}

// Copies one required float property into a per-vertex Vec3 column.
void fillVec3Column(Mesh& mesh, const char* name, const std::vector<float>& x,
                    const std::vector<float>& y, const std::vector<float>& z,
                    const std::vector<VertexId>& ids) {
    auto& column = mesh.vertexAttributes().create<Vec3>(name);
    for (std::size_t i = 0; i < ids.size() && i < x.size(); ++i) {
        column[ids[i].value] = {x[i], y[i], z[i]};
    }
}

struct PlyVertexPayload {
    std::vector<float> nx, ny, nz;
    std::vector<float> r, g, b;
    std::vector<float> mix;
};

Result<PlyVertexPayload> readVertexPayload(happly::PLYData& ply, std::string_view origin) {
    happly::Element& vertex = ply.getElement("vertex");
    static constexpr std::array<const char*, 7> kRequired{
        "nx", "ny", "nz", "red", "green", "blue", kMaterialMixAttribute};
    std::string missing;
    for (const char* name : kRequired) {
        if (!vertex.hasProperty(name)) {
            missing += missing.empty() ? "" : ", ";
            missing += name;
        }
    }
    // Every missing property at once: fixing a producer one round-trip per
    // property is exactly the kind of loop a loud error should collapse.
    if (!missing.empty()) {
        return Error{ErrorCode::ParseError,
                     std::string(origin) + ": handoff is missing required vertex propert" +
                         (missing.find(',') == std::string::npos ? "y " : "ies ") + missing};
    }
    PlyVertexPayload payload;
    payload.nx = vertex.getProperty<float>("nx");
    payload.ny = vertex.getProperty<float>("ny");
    payload.nz = vertex.getProperty<float>("nz");
    // uchar 0..255 by profile (happly widens narrower integer types into it),
    // scaled into the engine's 0..1 color convention.
    const std::vector<unsigned char> red = vertex.getProperty<unsigned char>("red");
    const std::vector<unsigned char> green = vertex.getProperty<unsigned char>("green");
    const std::vector<unsigned char> blue = vertex.getProperty<unsigned char>("blue");
    payload.r.reserve(red.size());
    payload.g.reserve(green.size());
    payload.b.reserve(blue.size());
    for (std::size_t i = 0; i < red.size(); ++i) {
        payload.r.push_back(static_cast<float>(red[i]) / 255.0f);
        payload.g.push_back(static_cast<float>(green[i]) / 255.0f);
        payload.b.push_back(static_cast<float>(blue[i]) / 255.0f);
    }
    payload.mix = vertex.getProperty<float>(kMaterialMixAttribute);
    return payload;
}

Result<Handoff> buildFromPly(happly::PLYData& ply, const HeaderDeclaration& header,
                             std::string_view origin) {
    const std::vector<std::array<double, 3>> positions = ply.getVertexPositions();
    if (positions.empty()) {
        return Error{ErrorCode::EmptyMesh, std::string(origin) + ": handoff has no vertices"};
    }
    Result<PlyVertexPayload> payload = readVertexPayload(ply, origin);
    if (!payload.ok()) {
        return payload.error();
    }
    const std::vector<std::vector<std::size_t>> faces = ply.getFaceIndices<std::size_t>();

    Handoff out;
    out.version = header.version;
    out.producer = header.producer;
    std::vector<VertexId> ids;
    ids.reserve(positions.size());
    for (const auto& p : positions) {
        ids.push_back(out.mesh.addVertex(
            {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])}));
    }

    for (const auto& face : faces) {
        if (face.size() != 3) {
            return Error{ErrorCode::ParseError, std::string(origin) + ": handoff face has " +
                                                    std::to_string(face.size()) +
                                                    " vertices; the profile requires triangles"};
        }
        std::array<VertexId, 3> tri{};
        for (std::size_t k = 0; k < 3; ++k) {
            if (face[k] >= ids.size()) {
                return Error{ErrorCode::ParseError,
                             std::string(origin) + ": handoff face references vertex " +
                                 std::to_string(face[k]) + " of " + std::to_string(ids.size())};
            }
            tri[k] = ids[face[k]];
        }
        if (!out.mesh.addFace(tri).valid()) {
            ++out.droppedFaces;
        }
    }
    if (out.mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, std::string(origin) + ": handoff has no usable faces"};
    }
    reportDroppedFaces(out, origin, faces.size());

    const PlyVertexPayload& v = payload.value();
    fillVec3Column(out.mesh, io::kNormalAttribute, v.nx, v.ny, v.nz, ids);
    fillVec3Column(out.mesh, io::kColorAttribute, v.r, v.g, v.b, ids);
    auto& mix = out.mesh.vertexAttributes().create<float>(kMaterialMixAttribute);
    for (std::size_t i = 0; i < ids.size() && i < v.mix.size(); ++i) {
        mix[ids[i].value] = v.mix[i];
    }
    computeBounds(out);
    return out;
}

Result<Handoff> readPlyBytes(std::string_view bytes, std::string_view origin) {
    // Version FIRST, straight off the header text: an incompatible handoff must
    // be rejected on its contract before a single vertex exists, whatever else
    // is wrong with the payload.
    const HeaderDeclaration header = scanComments(headerComments(bytes));
    if (!header.found) {
        return Error{ErrorCode::UnsupportedFormat,
                     std::string(origin) + ": not a sculpt handoff (no \"comment " +
                         kVersionComment + " <major> <minor>\" in the PLY header)"};
    }
    if (header.malformed) {
        return Error{ErrorCode::IncompatibleVersion,
                     std::string(origin) + ": malformed \"" + kVersionComment +
                         "\" version declaration (this engine supports " + supportedText() + ")"};
    }
    if (!versionSupported(header.version)) {
        return versionRejection(origin, header.version);
    }

    try {
        std::istringstream stream{std::string(bytes)};
        happly::PLYData ply(stream);
        ply.validate();
        return buildFromPly(ply, header, origin);
    } catch (const std::exception& e) {
        return Error{ErrorCode::ParseError,
                     std::string(origin) + ": failed to parse handoff: " + e.what()};
    }
}

// glTF handoffs declare their version in asset.extras.cyberSculptHandoff. The
// version is read out of the JSON text directly so the gate stays ahead of the
// geometry import, exactly as the PLY route does.
struct GltfDeclaration {
    bool found = false;
    Version version;
    std::string producer;
};

std::optional<int> readJsonInt(std::string_view text, std::string_view key) {
    const std::size_t at = text.find(std::string("\"") + std::string(key) + "\"");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t i = text.find(':', at);
    if (i == std::string_view::npos) {
        return std::nullopt;
    }
    ++i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
    }
    std::string digits;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
        digits.push_back(text[i]);
        ++i;
    }
    return parseInt(digits);
}

// The value of a JSON string member. Backslash escapes are unescaped by taking
// the next character literally, which is enough for a free-form producer label.
std::optional<std::string> readJsonString(std::string_view text, std::string_view key) {
    const std::size_t at = text.find(std::string("\"") + std::string(key) + "\"");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t i = text.find(':', at);
    if (i == std::string_view::npos) {
        return std::nullopt;
    }
    i = text.find('"', i);
    if (i == std::string_view::npos) {
        return std::nullopt;
    }
    std::string value;
    for (++i; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
        }
        value.push_back(text[i]);
    }
    return value;
}

// The cyberSculptHandoff object as text, from its opening brace to the matching
// close. Scanning only that span keeps "major"/"minor"/"producer" from matching
// an unrelated member elsewhere in a large glTF document.
std::optional<std::string_view> handoffObject(std::string_view text) {
    const std::size_t key = text.find("cyberSculptHandoff");
    if (key == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t open = text.find('{', key);
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    int depth = 0;
    bool inString = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            inString = c != '"';
            i += (c == '\\') ? 1 : 0;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return text.substr(open, i - open + 1);
        }
    }
    return std::nullopt;
}

GltfDeclaration scanGltf(std::string_view text) {
    GltfDeclaration out;
    const std::optional<std::string_view> object = handoffObject(text);
    if (!object) {
        return out;
    }
    const std::optional<int> major = readJsonInt(*object, "major");
    const std::optional<int> minor = readJsonInt(*object, "minor");
    if (!major || !minor) {
        return out;
    }
    out.found = true;
    out.version = Version{*major, *minor};
    out.producer = readJsonString(*object, "producer").value_or(std::string());
    return out;
}

// glTF stores NORMAL per vertex, but the engine's importer scatters it onto
// mesh CORNERS. A handoff reports and consumes VERTEX normals, so fold the
// corner column back: every corner of a vertex was written from the same glTF
// vertex's NORMAL, which makes the copy exact rather than an average.
void promoteCornerNormals(Mesh& mesh) {
    const auto* corners = mesh.cornerAttributes().find<Vec3>(io::kNormalAttribute);
    if (corners == nullptr || mesh.vertexAttributes().find<Vec3>(io::kNormalAttribute) != nullptr) {
        return;
    }
    auto& vertexNormals = mesh.vertexAttributes().create<Vec3>(io::kNormalAttribute);
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        if (!mesh.isAlive(FaceId{f})) {
            continue;
        }
        for (const LoopId loop : mesh.faceLoops(FaceId{f})) {
            vertexNormals[mesh.loopVertex(loop).value] = (*corners)[loop.value];
        }
    }
}

Result<Handoff> readGltfFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error{ErrorCode::FileNotFound, "cannot read '" + path.string() + "'"};
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    const GltfDeclaration declaration = scanGltf(bytes);
    if (!declaration.found) {
        return Error{
            ErrorCode::UnsupportedFormat,
            path.string() + ": not a sculpt handoff (no asset.extras.cyberSculptHandoff version)"};
    }
    if (!versionSupported(declaration.version)) {
        return versionRejection(path.string(), declaration.version);
    }
    auto imported = io::importMesh(path);
    if (!imported.ok()) {
        return imported.error();
    }
    Handoff out;
    out.version = declaration.version;
    out.producer = declaration.producer;
    out.mesh = std::move(imported.value().mesh);
    out.boundsMin = imported.value().boundsMin;
    out.boundsMax = imported.value().boundsMax;
    out.warnings = imported.value().warnings;
    out.droppedFaces = imported.value().droppedFaces;
    promoteCornerNormals(out.mesh);
    // Every payload the format doc lists as required but this container did not
    // deliver is named. material_mix in particular has no glTF core slot, so it
    // is a warning rather than a rejection — it is the one payload the format
    // cannot carry natively.
    const auto warnMissing = [&out](bool present, const std::string& what) {
        if (!present) {
            out.warnings.emplace_back("glTF handoff carries no " + what + " payload");
        }
    };
    warnMissing(out.hasVertexNormals(), "per-vertex normal");
    warnMissing(out.hasVertexColors(), "vertex color");
    warnMissing(out.hasMaterialMix(), kMaterialMixAttribute);
    if (out.droppedFaces > 0) {
        out.warnings.emplace_back(path.string() + ": dropped " + std::to_string(out.droppedFaces) +
                                  " glTF triangle(s) the mesh refused");
    }
    return out;
}

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

Result<Handoff> readBytes(std::string_view bytes, std::string_view originLabel) {
    return readPlyBytes(bytes, originLabel);
}

Result<Handoff> readStream(std::istream& stream, std::string_view originLabel) {
    const std::string bytes((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return Error{ErrorCode::EmptyMesh, std::string(originLabel) + ": handoff stream was empty"};
    }
    return readPlyBytes(bytes, originLabel);
}

Result<Handoff> readFile(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    if (ext == ".gltf" || ext == ".glb") {
        return readGltfFile(path);
    }
    if (ext != ".ply") {
        return Error{ErrorCode::UnsupportedFormat,
                     "'" + path.string() + "': sculpt handoffs are .ply (normative) or .gltf/.glb"};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error{ErrorCode::FileNotFound, "cannot read '" + path.string() + "'"};
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    return readPlyBytes(bytes, path.string());
}

Result<Handoff> readBuffers(const BufferView& buffers) {
    constexpr const char* kOrigin = "<buffers>";
    if (!versionSupported(buffers.version)) {
        return versionRejection(kOrigin, buffers.version);
    }
    if (buffers.positions == nullptr || buffers.vertexCount == 0) {
        return Error{ErrorCode::EmptyMesh, "handoff buffers carry no vertices"};
    }
    if (buffers.indices == nullptr || buffers.indexCount == 0 || buffers.indexCount % 3 != 0) {
        return Error{ErrorCode::ParseError,
                     "handoff buffers need a triangle index array (indexCount % 3 == 0), got " +
                         std::to_string(buffers.indexCount)};
    }

    Handoff out;
    out.version = buffers.version;
    if (buffers.producer != nullptr) {
        out.producer = buffers.producer;
    }
    std::vector<VertexId> ids;
    ids.reserve(buffers.vertexCount);
    for (std::size_t i = 0; i < buffers.vertexCount; ++i) {
        ids.push_back(
            out.mesh.addVertex({buffers.positions[i * 3 + 0], buffers.positions[i * 3 + 1],
                                buffers.positions[i * 3 + 2]}));
    }
    for (std::size_t i = 0; i < buffers.indexCount; i += 3) {
        std::array<VertexId, 3> tri{};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t index = buffers.indices[i + k];
            if (index >= buffers.vertexCount) {
                return Error{ErrorCode::ParseError, "handoff buffers reference vertex " +
                                                        std::to_string(index) + " of " +
                                                        std::to_string(buffers.vertexCount)};
            }
            tri[k] = ids[index];
        }
        if (!out.mesh.addFace(tri).valid()) {
            ++out.droppedFaces;
        }
    }
    if (out.mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, "handoff buffers produced no usable faces"};
    }
    reportDroppedFaces(out, kOrigin, buffers.indexCount / 3);

    const auto fillVec3 = [&](const char* name, const float* source) {
        if (source == nullptr) {
            return;
        }
        auto& column = out.mesh.vertexAttributes().create<Vec3>(name);
        for (std::size_t i = 0; i < buffers.vertexCount; ++i) {
            column[ids[i].value] = {source[i * 3 + 0], source[i * 3 + 1], source[i * 3 + 2]};
        }
    };
    fillVec3(io::kNormalAttribute, buffers.normals);
    fillVec3(io::kColorAttribute, buffers.colors);
    if (buffers.materialMix != nullptr) {
        auto& mix = out.mesh.vertexAttributes().create<float>(kMaterialMixAttribute);
        for (std::size_t i = 0; i < buffers.vertexCount; ++i) {
            mix[ids[i].value] = buffers.materialMix[i];
        }
    }
    computeBounds(out);
    return out;
}

}  // namespace cyber::handoff
