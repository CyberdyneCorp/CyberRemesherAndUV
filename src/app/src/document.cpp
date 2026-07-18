#include "cyber/app/document.hpp"

#include <cstddef>
#include <utility>

namespace cyber::app {
namespace {

enum class SectionId : std::uint32_t {
    Target = 1,
    EditMesh = 2,
    Parameters = 3,
    BakeState = 4,
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
    ByteWriter w;
    w.u32(kMagic);
    w.u32(kFormatVersion);
    w.u32(4u);  // section count
    writeSection(w, SectionId::Target, serializeMesh(target));
    writeSection(w, SectionId::EditMesh, serializeMesh(editMesh));
    writeSection(w, SectionId::Parameters, serializeParams(params));
    writeSection(w, SectionId::BakeState, serializeBake(bake));
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
    return sameMesh(a.target, b.target) && sameMesh(a.editMesh, b.editMesh) &&
           sameParams(a.params, b.params) && a.bake == b.bake;
}

}  // namespace cyber::app
