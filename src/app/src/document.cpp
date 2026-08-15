#include "cyber/app/document.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cyber::app {
namespace {

enum class SectionId : std::uint32_t {
    Target = 1,
    EditMesh = 2,
    Parameters = 3,
    BakeState = 4,
    SoftSelections = 5,
    TargetExtras = 6,
    EditMeshExtras = 7,
};

std::vector<std::uint8_t> serializeMesh(const Mesh& mesh) {
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(positions, faces);

    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(positions.size()));
    for (const Vec3 p : positions) {
        w.f32(p.x);
        w.f32(p.y);
        w.f32(p.z);
    }
    w.u32(static_cast<std::uint32_t>(faces.size()));
    for (const auto& face : faces) {
        w.u32(static_cast<std::uint32_t>(face.size()));
        for (const Index vi : face) {
            w.u32(vi);
        }
    }
    return w.take();
}

std::optional<Mesh> readMesh(ByteReader& r) {
    // Validate every length field against the bytes actually left before
    // reserving — a corrupt/malicious file (e.g. vertexCount = 0xFFFFFFFF)
    // would otherwise throw bad_alloc/length_error and abort. Each vertex is
    // 12 bytes (3 floats); each face is at least its 4-byte arity field.
    const std::uint32_t vertexCount = r.u32();
    if (vertexCount > r.remaining() / 12u) {
        return std::nullopt;
    }
    std::vector<Vec3> positions;
    positions.reserve(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        const float x = r.f32();
        const float y = r.f32();
        const float z = r.f32();
        positions.push_back(Vec3{x, y, z});
    }

    const std::uint32_t faceCount = r.u32();
    if (faceCount > r.remaining() / 4u) {
        return std::nullopt;
    }
    std::vector<std::vector<Index>> faces;
    faces.reserve(faceCount);
    for (std::uint32_t i = 0; i < faceCount; ++i) {
        const std::uint32_t arity = r.u32();
        if (arity > r.remaining() / 4u) {
            return std::nullopt;
        }
        std::vector<Index> face;
        face.reserve(arity);
        for (std::uint32_t j = 0; j < arity; ++j) {
            face.push_back(r.u32());
        }
        faces.push_back(std::move(face));
    }

    if (!r.ok()) {
        return std::nullopt;
    }
    return Mesh::fromIndexed(positions, faces);
}

std::vector<std::uint8_t> serializeParams(const remesh::Parameters& p) {
    ByteWriter w;
    w.i32(p.targetQuadCount);
    w.f32(p.edgeScale);
    w.f32(p.sharpEdgeDegrees);
    w.f32(p.smoothNormalDegrees);
    w.f32(p.adaptivity);
    w.u8(p.pureQuads ? 1u : 0u);
    w.i32(p.holeFillMaxBoundary);
    w.u32(static_cast<std::uint32_t>(p.smallPatchPolicy));
    w.i32(p.smallPatchMinFaces);
    return w.take();
}

remesh::Parameters readParams(ByteReader& r) {
    remesh::Parameters p;
    p.targetQuadCount = r.i32();
    p.edgeScale = r.f32();
    p.sharpEdgeDegrees = r.f32();
    p.smoothNormalDegrees = r.f32();
    p.adaptivity = r.f32();
    p.pureQuads = r.u8() != 0;
    p.holeFillMaxBoundary = r.i32();
    p.smallPatchPolicy = static_cast<remesh::SmallPatchPolicy>(r.u32());
    p.smallPatchMinFaces = r.i32();
    return p;
}

std::vector<std::uint8_t> serializeBake(const BakeState& b) {
    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(b.map));
    w.i32(b.width);
    w.i32(b.height);
    w.f32(b.cageDistance);
    w.i32(b.samples);
    w.u8(b.baked ? 1u : 0u);
    w.u64(b.bakeRevision);
    return w.take();
}

BakeState readBake(ByteReader& r) {
    BakeState b;
    b.map = static_cast<BakeMapKind>(r.u32());
    b.width = r.i32();
    b.height = r.i32();
    b.cageDistance = r.f32();
    b.samples = r.i32();
    b.baked = r.u8() != 0;
    b.bakeRevision = r.u64();
    return b;
}

// Slot weights are indexed by EditMesh vertex id, but the mesh itself is
// written through toIndexed, which drops dead vertices and renumbers the alive
// ones 0..n-1. Rebase the slots onto that same numbering so both halves of the
// container agree: on load, fromIndexed hands back exactly that dense id space,
// so a weight lands on the vertex it was painted on. A mesh with no dead
// vertices already IS its own compaction, so its slots are passed through
// untouched and the bytes are unchanged.
std::map<std::string, std::vector<float>> compactSelections(
    const Mesh& mesh, const std::map<std::string, std::vector<float>>& slots) {
    if (mesh.vertexCount() == mesh.vertexCapacity()) {
        return slots;
    }
    std::vector<std::size_t> aliveIds;
    aliveIds.reserve(mesh.vertexCount());
    for (std::size_t i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{static_cast<Index>(i)})) {
            aliveIds.push_back(i);
        }
    }

    std::map<std::string, std::vector<float>> compact;
    for (const auto& [name, weights] : slots) {
        std::vector<float> rebased;
        rebased.reserve(aliveIds.size());
        for (const std::size_t id : aliveIds) {
            rebased.push_back(id < weights.size() ? weights[id] : 0.0f);
        }
        compact.emplace(name, std::move(rebased));
    }
    return compact;
}

std::vector<std::uint8_t> serializeSelections(
    const std::map<std::string, std::vector<float>>& slots) {
    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(slots.size()));
    for (const auto& [name, weights] : slots) {
        w.str(name);
        w.u32(static_cast<std::uint32_t>(weights.size()));
        for (const float value : weights) {
            w.f32(value);
        }
    }
    return w.take();
}

// Every count is validated against the bytes actually left before reserving,
// the same way readMesh guards its own length fields.
std::optional<std::map<std::string, std::vector<float>>> readSelections(ByteReader& r) {
    std::map<std::string, std::vector<float>> slots;
    const std::uint32_t slotCount = r.u32();
    if (!r.ok() || slotCount > r.remaining() / 8u) {  // >= 4-byte name + 4-byte count each
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < slotCount; ++i) {
        std::string name = r.str();
        const std::uint32_t weightCount = r.u32();
        if (!r.ok() || weightCount > r.remaining() / 4u) {
            return std::nullopt;
        }
        std::vector<float> weights;
        weights.reserve(weightCount);
        for (std::uint32_t j = 0; j < weightCount; ++j) {
            weights.push_back(r.f32());
        }
        slots.emplace(std::move(name), std::move(weights));
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    return slots;
}

// ---- mesh attributes and feature edges (sections 6/7) ----------------------
// Attribute columns are keyed by element id, but a mesh is written through
// toIndexed, which drops dead elements and renumbers the survivors. Rows are
// written in that same compacted order — vertices by id, faces by id, corners
// in each face's loop order — so `load` puts every row back on the element
// fromIndexed rebuilt from it. Edges have no place in the indexed form, so an
// edge row is stored as the compacted vertex pair it spans (plus its feature
// flag) and resolved with edgeBetween on load.

constexpr std::uint8_t kColumnFloat = 0;
constexpr std::uint8_t kColumnInt32 = 1;
constexpr std::uint8_t kColumnVec2 = 2;
constexpr std::uint8_t kColumnVec3 = 3;
constexpr std::uint8_t kColumnVec4 = 4;

// Element id standing for "this row has no element in the rebuilt mesh".
constexpr std::size_t kNoElement = static_cast<std::size_t>(-1);

template <typename T>
constexpr std::uint8_t columnTag() {
    if constexpr (std::is_same_v<T, float>) {
        return kColumnFloat;
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return kColumnInt32;
    } else if constexpr (std::is_same_v<T, Vec2>) {
        return kColumnVec2;
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return kColumnVec3;
    } else {
        return kColumnVec4;
    }
}

template <typename T>
constexpr std::size_t columnValueBytes() {
    if constexpr (std::is_same_v<T, Vec2>) {
        return 8;
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return 12;
    } else if constexpr (std::is_same_v<T, Vec4>) {
        return 16;
    } else {
        return 4;
    }
}

void writeValue(ByteWriter& w, float v) { w.f32(v); }
void writeValue(ByteWriter& w, std::int32_t v) { w.i32(v); }
void writeValue(ByteWriter& w, Vec2 v) {
    w.f32(v.x);
    w.f32(v.y);
}
void writeValue(ByteWriter& w, Vec3 v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
}
void writeValue(ByteWriter& w, Vec4 v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
    w.f32(v.w);
}

template <typename T>
T readValue(ByteReader& r) {
    if constexpr (std::is_same_v<T, float>) {
        return r.f32();
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return r.i32();
    } else {
        T v{};
        v.x = r.f32();
        v.y = r.f32();
        if constexpr (!std::is_same_v<T, Vec2>) {
            v.z = r.f32();
        }
        if constexpr (std::is_same_v<T, Vec4>) {
            v.w = r.f32();
        }
        return v;
    }
}

// `order[k]` is the element id whose row is written at position k.
template <typename T>
void writeColumn(ByteWriter& w, const std::string& name, const std::vector<T>& values,
                 const std::vector<std::size_t>& order) {
    w.str(name);
    w.u8(columnTag<T>());
    w.u32(static_cast<std::uint32_t>(order.size()));
    for (const std::size_t id : order) {
        writeValue(w, id < values.size() ? values[id] : T{});
    }
}

void writeAttributeSet(ByteWriter& w, const AttributeSet& attrs,
                       const std::vector<std::size_t>& order) {
    w.u32(static_cast<std::uint32_t>(attrs.columnCount()));
    attrs.forEachColumn(
        [&](const std::string& name, const auto& values) { writeColumn(w, name, values, order); });
}

template <typename T>
bool readColumn(ByteReader& r, AttributeSet& attrs, const std::string& name,
                const std::vector<std::size_t>& order) {
    // A column always carries one row per element of the mesh it was written
    // with. Demanding that keeps a forged file from declaring many columns of
    // few bytes each: every column now costs its rows on the wire, so the
    // memory a load can be made to allocate stays proportional to the file.
    const std::uint32_t rows = r.u32();
    if (!r.ok() || rows != order.size() || rows > r.remaining() / columnValueBytes<T>()) {
        return false;
    }
    std::vector<T>& column = attrs.create<T>(name);
    for (std::uint32_t i = 0; i < rows; ++i) {
        const T value = readValue<T>(r);
        if (order[i] < column.size()) {
            column[order[i]] = value;
        }
    }
    return r.ok();
}

bool readColumnOfTag(ByteReader& r, AttributeSet& attrs, const std::string& name,
                     std::uint8_t tag, const std::vector<std::size_t>& order) {
    switch (tag) {
        case kColumnFloat:
            return readColumn<float>(r, attrs, name, order);
        case kColumnInt32:
            return readColumn<std::int32_t>(r, attrs, name, order);
        case kColumnVec2:
            return readColumn<Vec2>(r, attrs, name, order);
        case kColumnVec3:
            return readColumn<Vec3>(r, attrs, name, order);
        case kColumnVec4:
            return readColumn<Vec4>(r, attrs, name, order);
        default:
            return false;
    }
}

bool readAttributeSet(ByteReader& r, AttributeSet& attrs, const std::vector<std::size_t>& order) {
    const std::uint32_t columns = r.u32();
    // Each column costs at least a 4-byte name length, a 1-byte type tag and a
    // 4-byte row count, so a count past that bound cannot be honest.
    if (!r.ok() || columns > r.remaining() / 9u) {
        return false;
    }
    for (std::uint32_t i = 0; i < columns; ++i) {
        const std::string name = r.str();
        const std::uint8_t tag = r.u8();
        if (!r.ok() || !readColumnOfTag(r, attrs, name, tag, order)) {
            return false;
        }
    }
    return r.ok();
}

// Element ids in the order toIndexed exports them.
struct ElementOrder {
    std::vector<std::size_t> vertices;
    std::vector<std::size_t> faces;
    std::vector<std::size_t> corners;
};

ElementOrder compactOrder(const Mesh& mesh) {
    ElementOrder order;
    order.vertices.reserve(mesh.vertexCount());
    for (std::size_t i = 0; i < mesh.vertexCapacity(); ++i) {
        if (mesh.isAlive(VertexId{static_cast<Index>(i)})) {
            order.vertices.push_back(i);
        }
    }
    order.faces.reserve(mesh.faceCount());
    for (std::size_t i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId face{static_cast<Index>(i)};
        if (!mesh.isAlive(face)) {
            continue;
        }
        order.faces.push_back(i);
        for (const LoopId loop : mesh.faceLoops(face)) {
            order.corners.push_back(loop.value);
        }
    }
    return order;
}

// Edges worth persisting: the tagged ones always, plus every alive edge once
// the mesh carries edge columns (their rows are keyed to this same list).
std::vector<std::size_t> persistedEdges(const Mesh& mesh) {
    const bool all = mesh.edgeAttributes().columnCount() > 0;
    std::vector<std::size_t> ids;
    for (std::size_t i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId edge{static_cast<Index>(i)};
        if (mesh.isAlive(edge) && (all || mesh.isFeatureEdge(edge))) {
            ids.push_back(i);
        }
    }
    return ids;
}

bool hasMeshExtras(const Mesh& mesh) {
    const std::size_t columns = mesh.vertexAttributes().columnCount() +
                                mesh.edgeAttributes().columnCount() +
                                mesh.faceAttributes().columnCount() +
                                mesh.cornerAttributes().columnCount();
    return columns > 0 || !persistedEdges(mesh).empty();
}

std::vector<std::uint8_t> serializeMeshExtras(const Mesh& mesh) {
    const ElementOrder order = compactOrder(mesh);
    std::vector<Index> compactVertex(mesh.vertexCapacity(), kInvalidIndex);
    for (std::size_t k = 0; k < order.vertices.size(); ++k) {
        compactVertex[order.vertices[k]] = static_cast<Index>(k);
    }

    ByteWriter w;
    writeAttributeSet(w, mesh.vertexAttributes(), order.vertices);
    writeAttributeSet(w, mesh.faceAttributes(), order.faces);
    writeAttributeSet(w, mesh.cornerAttributes(), order.corners);

    const std::vector<std::size_t> edges = persistedEdges(mesh);
    w.u32(static_cast<std::uint32_t>(edges.size()));
    for (const std::size_t id : edges) {
        const EdgeId edge{static_cast<Index>(id)};
        const auto [v0, v1] = mesh.edgeVertices(edge);
        w.u32(compactVertex[v0.value]);
        w.u32(compactVertex[v1.value]);
        w.u8(mesh.isFeatureEdge(edge) ? 1u : 0u);
    }
    writeAttributeSet(w, mesh.edgeAttributes(), edges);
    return w.take();
}

// Reads the edge records onto `mesh` and returns the edge id each row belongs
// to (kNoElement when the pair names no edge of the rebuilt mesh).
std::optional<std::vector<std::size_t>> readEdgeRecords(ByteReader& r, Mesh& mesh) {
    const std::uint32_t count = r.u32();
    if (!r.ok() || count > r.remaining() / 9u) {  // two 4-byte ids + 1-byte flag each
        return std::nullopt;
    }
    std::vector<std::size_t> ids;
    ids.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Index a = r.u32();
        const Index b = r.u32();
        const bool feature = r.u8() != 0;
        if (!r.ok()) {
            return std::nullopt;
        }
        const EdgeId edge = mesh.edgeBetween(VertexId{a}, VertexId{b});
        if (!edge.valid()) {
            ids.push_back(kNoElement);
            continue;
        }
        mesh.setFeatureEdge(edge, feature);
        ids.push_back(edge.value);
    }
    return ids;
}

bool readMeshExtras(ByteReader& r, Mesh& mesh) {
    const ElementOrder order = compactOrder(mesh);
    if (!readAttributeSet(r, mesh.vertexAttributes(), order.vertices) ||
        !readAttributeSet(r, mesh.faceAttributes(), order.faces) ||
        !readAttributeSet(r, mesh.cornerAttributes(), order.corners)) {
        return false;
    }
    const auto edges = readEdgeRecords(r, mesh);
    if (!edges) {
        return false;
    }
    return readAttributeSet(r, mesh.edgeAttributes(), *edges);
}

void writeSection(ByteWriter& w, SectionId id, const std::vector<std::uint8_t>& payload) {
    w.u32(static_cast<std::uint32_t>(id));
    w.u64(static_cast<std::uint64_t>(payload.size()));
    w.bytes(payload);
}

bool sameMesh(const Mesh& a, const Mesh& b) {
    std::vector<Vec3> pa, pb;
    std::vector<std::vector<Index>> fa, fb;
    a.toIndexed(pa, fa);
    b.toIndexed(pb, fb);
    return pa == pb && fa == fb;
}

bool sameParams(const remesh::Parameters& a, const remesh::Parameters& b) {
    return a.targetQuadCount == b.targetQuadCount && a.edgeScale == b.edgeScale &&
           a.sharpEdgeDegrees == b.sharpEdgeDegrees &&
           a.smoothNormalDegrees == b.smoothNormalDegrees && a.adaptivity == b.adaptivity &&
           a.pureQuads == b.pureQuads && a.holeFillMaxBoundary == b.holeFillMaxBoundary &&
           a.smallPatchPolicy == b.smallPatchPolicy && a.smallPatchMinFaces == b.smallPatchMinFaces;
}

}  // namespace

std::vector<std::uint8_t> Document::save() const {
    // Optional sections are written only when they carry data, so a document
    // that uses none of them is byte-identical to what earlier builds wrote.
    std::vector<std::pair<SectionId, std::vector<std::uint8_t>>> sections;
    sections.emplace_back(SectionId::Target, serializeMesh(target));
    sections.emplace_back(SectionId::EditMesh, serializeMesh(editMesh));
    sections.emplace_back(SectionId::Parameters, serializeParams(params));
    sections.emplace_back(SectionId::BakeState, serializeBake(bake));
    if (!softSelections.empty()) {
        sections.emplace_back(SectionId::SoftSelections,
                              serializeSelections(compactSelections(editMesh, softSelections)));
    }
    if (hasMeshExtras(target)) {
        sections.emplace_back(SectionId::TargetExtras, serializeMeshExtras(target));
    }
    if (hasMeshExtras(editMesh)) {
        sections.emplace_back(SectionId::EditMeshExtras, serializeMeshExtras(editMesh));
    }

    ByteWriter w;
    w.u32(kMagic);
    w.u32(kFormatVersion);
    w.u32(static_cast<std::uint32_t>(sections.size()));
    for (const auto& [id, payload] : sections) {
        writeSection(w, id, payload);
    }
    return w.take();
}

std::optional<Document> Document::load(std::span<const std::uint8_t> bytes) {
    ByteReader r(bytes);
    const std::uint32_t magic = r.u32();
    const std::uint32_t version = r.u32();
    const std::uint32_t sectionCount = r.u32();
    if (!r.ok() || magic != kMagic || version > kFormatVersion) {
        return std::nullopt;
    }

    Document doc;
    for (std::uint32_t i = 0; i < sectionCount; ++i) {
        const std::uint32_t id = r.u32();
        const std::uint64_t length = r.u64();
        if (!r.ok()) {
            return std::nullopt;
        }
        // Reject the declared length before narrowing it: a 64-bit length that
        // exceeds the file cannot name a real section, and the cast to size_t
        // would otherwise truncate it into a plausible one where size_t is
        // narrower than 64 bits.
        if (length > r.remaining()) {
            return std::nullopt;
        }
        ByteReader section = r.sub(static_cast<std::size_t>(length));
        if (!r.ok()) {
            return std::nullopt;
        }
        switch (static_cast<SectionId>(id)) {
            case SectionId::Target: {
                auto mesh = readMesh(section);
                if (!mesh) {
                    return std::nullopt;
                }
                doc.target = std::move(*mesh);
                break;
            }
            case SectionId::EditMesh: {
                auto mesh = readMesh(section);
                if (!mesh) {
                    return std::nullopt;
                }
                doc.editMesh = std::move(*mesh);
                break;
            }
            case SectionId::Parameters:
                doc.params = readParams(section);
                if (!section.ok()) {
                    return std::nullopt;
                }
                break;
            case SectionId::BakeState:
                doc.bake = readBake(section);
                if (!section.ok()) {
                    return std::nullopt;
                }
                break;
            case SectionId::SoftSelections: {
                auto slots = readSelections(section);
                if (!slots) {
                    return std::nullopt;
                }
                doc.softSelections = std::move(*slots);
                break;
            }
            case SectionId::TargetExtras:
                if (!readMeshExtras(section, doc.target)) {
                    return std::nullopt;
                }
                break;
            case SectionId::EditMeshExtras:
                if (!readMeshExtras(section, doc.editMesh)) {
                    return std::nullopt;
                }
                break;
            default:
                break;  // unknown section: skipped by its length
        }
    }
    return doc;
}

bool Document::autosaveIfDirty(const AutosaveSink& sink) {
    if (!m_dirty) {
        return false;
    }
    const std::vector<std::uint8_t> buffer = save();
    if (sink) {
        sink(buffer);
    }
    m_dirty = false;
    return true;
}

bool operator==(const Document& a, const Document& b) {
    // Slots are compared in the same compacted id space the meshes are, so a
    // document still equals its own save->load round trip after a deletion.
    return sameMesh(a.target, b.target) && sameMesh(a.editMesh, b.editMesh) &&
           sameParams(a.params, b.params) && a.bake == b.bake &&
           compactSelections(a.editMesh, a.softSelections) ==
               compactSelections(b.editMesh, b.softSelections);
}

}  // namespace cyber::app
