#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <tuple>

#include "io_internal.hpp"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

namespace cyber::io::detail {

namespace {

constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

bool addOverflows(std::size_t a, std::size_t b) { return a > kSizeMax - b; }
bool mulOverflows(std::size_t a, std::size_t b) { return b != 0 && a > kSizeMax / b; }

// The byte span an accessor declares must lie inside both its bufferView and
// the decoded buffer: count, stride and the offsets all come straight from the
// file and tinygltf never compares them against the data it decoded.
bool accessorFitsBuffer(const tinygltf::Accessor& accessor, const tinygltf::BufferView& view,
                        std::size_t bufferBytes, std::size_t stride, std::size_t elementBytes) {
    if (addOverflows(view.byteOffset, accessor.byteOffset)) {
        return false;
    }
    const std::size_t base = view.byteOffset + accessor.byteOffset;
    if (accessor.byteOffset > view.byteLength || base > bufferBytes) {
        return false;
    }
    if (accessor.count == 0) {
        return true;
    }
    const std::size_t last = accessor.count - 1;
    if (mulOverflows(last, stride) || addOverflows(last * stride, elementBytes)) {
        return false;
    }
    const std::size_t span = last * stride + elementBytes;  // bytes read past accessor.byteOffset
    if (addOverflows(accessor.byteOffset, span) || accessor.byteOffset + span > view.byteLength) {
        return false;
    }
    return !addOverflows(base, span) && base + span <= bufferBytes;
}

// Reads accessor element `i`, component `c`, converted to float with
// normalization for integer component types (glTF 2.0 spec).
class AccessorReader {
public:
    // The only way to build a reader: every index and byte range the accessor
    // implies is checked here, before any pointer is formed. tinygltf leaves
    // mesh accessor/bufferView/buffer indices unvalidated (an omitted
    // "bufferView" stays -1) and never bounds-checks the declared count, so a
    // malformed file would otherwise read far outside the decoded buffer.
    // Returns nullopt when the accessor cannot be read safely.
    [[nodiscard]] static std::optional<AccessorReader> make(const tinygltf::Model& model,
                                                            int accessorIndex,
                                                            std::size_t minComponents) {
        if (accessorIndex < 0 ||
            static_cast<std::size_t>(accessorIndex) >= model.accessors.size()) {
            return std::nullopt;
        }
        const tinygltf::Accessor& accessor =
            model.accessors[static_cast<std::size_t>(accessorIndex)];
        if (accessor.bufferView < 0 ||
            static_cast<std::size_t>(accessor.bufferView) >= model.bufferViews.size()) {
            return std::nullopt;
        }
        const tinygltf::BufferView& view =
            model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        if (view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= model.buffers.size()) {
            return std::nullopt;
        }
        const std::vector<unsigned char>& data =
            model.buffers[static_cast<std::size_t>(view.buffer)].data;

        const int stride = accessor.ByteStride(view);  // -1 on an unusable configuration
        const int componentBytes =
            tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(accessor.componentType));
        const int components =
            tinygltf::GetNumComponentsInType(static_cast<std::uint32_t>(accessor.type));
        if (stride <= 0 || componentBytes <= 0 || components <= 0 ||
            static_cast<std::size_t>(components) < minComponents) {
            return std::nullopt;
        }
        const std::size_t elementBytes =
            static_cast<std::size_t>(componentBytes) * static_cast<std::size_t>(components);
        if (!accessorFitsBuffer(accessor, view, data.size(), static_cast<std::size_t>(stride),
                                elementBytes)) {
            return std::nullopt;
        }
        return AccessorReader(accessor, data.data() + view.byteOffset + accessor.byteOffset,
                              static_cast<std::size_t>(stride),
                              static_cast<std::size_t>(components));
    }

    [[nodiscard]] std::size_t count() const { return m_accessor.count; }
    [[nodiscard]] std::size_t components() const { return m_components; }

    [[nodiscard]] float number(std::size_t i, std::size_t c) const {
        const unsigned char* p = m_data + i * m_stride;
        switch (m_accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT: {
                float v;
                std::memcpy(&v, p + c * 4, 4);
                return v;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return static_cast<float>(p[c]) / 255.0f;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                std::uint16_t v;
                std::memcpy(&v, p + c * 2, 2);
                return static_cast<float>(v) / 65535.0f;
            }
            default:
                return 0.0f;
        }
    }

    [[nodiscard]] std::uint32_t index(std::size_t i) const {
        const unsigned char* p = m_data + i * m_stride;
        switch (m_accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                std::uint32_t v;
                std::memcpy(&v, p, 4);
                return v;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                std::uint16_t v;
                std::memcpy(&v, p, 2);
                return v;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return *p;
            default:
                return 0;
        }
    }

private:
    AccessorReader(const tinygltf::Accessor& accessor, const unsigned char* data,
                   std::size_t stride, std::size_t components)
        : m_accessor(accessor), m_data(data), m_stride(stride), m_components(components) {}

    const tinygltf::Accessor& m_accessor;
    const unsigned char* m_data;
    std::size_t m_stride;
    std::size_t m_components;
};

int findAttribute(const tinygltf::Primitive& prim, const char* name) {
    const auto it = prim.attributes.find(name);
    return it == prim.attributes.end() ? -1 : it->second;
}

}  // namespace

Result<ImportedMesh> importGltf(const std::filesystem::path& path,
                                const ImportOptions& /*options*/) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    const bool binary = lowercaseExtension(path) == ".glb";
    const bool loaded = binary ? loader.LoadBinaryFromFile(&model, &err, &warn, path.string())
                               : loader.LoadASCIIFromFile(&model, &err, &warn, path.string());
    if (!loaded) {
        return Error{ErrorCode::ParseError, "failed to parse '" + path.string() + "': " + err};
    }

    ImportedMesh out;
    if (!warn.empty()) {
        out.warnings.push_back(warn);
    }
    std::size_t skipped = 0;

    auto invalidAccessor = [&path](const char* what, int index) {
        return Error{ErrorCode::ParseError, "accessor " + std::to_string(index) + " for " + what +
                                                " in '" + path.string() +
                                                "' is out of range or exceeds its buffer"};
    };

    for (const tinygltf::Mesh& gltfMesh : model.meshes) {
        for (const tinygltf::Primitive& prim : gltfMesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES) {
                out.warnings.push_back("skipped non-triangle primitive in '" + gltfMesh.name + "'");
                continue;
            }
            const int posAccessor = findAttribute(prim, "POSITION");
            if (posAccessor < 0) {
                continue;
            }
            const std::optional<AccessorReader> positions =
                AccessorReader::make(model, posAccessor, 3);
            if (!positions) {
                return invalidAccessor("POSITION", posAccessor);
            }
            std::vector<VertexId> ids;
            ids.reserve(positions->count());
            for (std::size_t i = 0; i < positions->count(); ++i) {
                ids.push_back(out.mesh.addVertex(
                    {positions->number(i, 0), positions->number(i, 1), positions->number(i, 2)}));
            }

            if (const int colorAccessor = findAttribute(prim, "COLOR_0"); colorAccessor >= 0) {
                const std::optional<AccessorReader> colors =
                    AccessorReader::make(model, colorAccessor, 3);
                if (!colors) {
                    return invalidAccessor("COLOR_0", colorAccessor);
                }
                auto& column = out.mesh.vertexAttributes().create<Vec3>(kColorAttribute);
                for (std::size_t i = 0; i < colors->count() && i < ids.size(); ++i) {
                    column[ids[i].value] = {colors->number(i, 0), colors->number(i, 1),
                                            colors->number(i, 2)};
                }
            }

            // Triangle list (indexed or sequential).
            std::vector<std::uint32_t> indices;
            if (prim.indices >= 0) {
                const std::optional<AccessorReader> reader =
                    AccessorReader::make(model, prim.indices, 1);
                if (!reader) {
                    return invalidAccessor("indices", prim.indices);
                }
                indices.reserve(reader->count());
                for (std::size_t i = 0; i < reader->count(); ++i) {
                    indices.push_back(reader->index(i));
                }
            } else {
                indices.resize(positions->count());
                for (std::uint32_t i = 0; i < indices.size(); ++i) {
                    indices[i] = i;
                }
            }

            const int uvAccessor = findAttribute(prim, "TEXCOORD_0");
            const int normalAccessor = findAttribute(prim, "NORMAL");
            const std::optional<AccessorReader> uvReader =
                uvAccessor >= 0 ? AccessorReader::make(model, uvAccessor, 2) : std::nullopt;
            if (uvAccessor >= 0 && !uvReader) {
                return invalidAccessor("TEXCOORD_0", uvAccessor);
            }
            const std::optional<AccessorReader> normalReader =
                normalAccessor >= 0 ? AccessorReader::make(model, normalAccessor, 3) : std::nullopt;
            if (normalAccessor >= 0 && !normalReader) {
                return invalidAccessor("NORMAL", normalAccessor);
            }
            // Read the optionals once, here: GCC cannot see that the engaged
            // check below dominates the member accesses and reports a bogus
            // -Wmaybe-uninitialized against the disengaged union storage.
            const AccessorReader* const uv = uvReader ? &*uvReader : nullptr;
            const AccessorReader* const normal = normalReader ? &*normalReader : nullptr;

            // Create the corner columns ONCE per primitive, like the OBJ reader
            // (io_obj.cpp): calling create() per triangle instead rebuilds a
            // size-m_size temporary every time — O(triangles * corners) = O(n^2).
            // The columns live in a node-based map, so these pointers stay valid
            // while addFace grows the columns below.
            std::vector<Vec2>* uvColumn =
                uv != nullptr ? &out.mesh.cornerAttributes().create<Vec2>(kUvAttribute) : nullptr;
            std::vector<Vec3>* normalColumn =
                normal != nullptr ? &out.mesh.cornerAttributes().create<Vec3>(kNormalAttribute)
                                  : nullptr;

            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                const std::uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
                if (i0 >= ids.size() || i1 >= ids.size() || i2 >= ids.size()) {
                    ++skipped;
                    continue;
                }
                const FaceId f = out.mesh.addFace(std::array{ids[i0], ids[i1], ids[i2]});
                if (!f.valid()) {
                    ++skipped;
                    continue;
                }
                if (uv != nullptr || normal != nullptr) {
                    const std::vector<LoopId> loops = out.mesh.faceLoops(f);
                    const std::uint32_t corner[3] = {i0, i1, i2};
                    if (uv != nullptr) {
                        auto& uvs = *uvColumn;
                        for (int c = 0; c < 3; ++c) {
                            // An attribute accessor may declare fewer elements than POSITION.
                            const std::size_t src = corner[c];
                            if (src >= uv->count()) {
                                continue;
                            }
                            uvs[loops[static_cast<std::size_t>(c)].value] = {uv->number(src, 0),
                                                                            uv->number(src, 1)};
                        }
                    }
                    if (normal != nullptr) {
                        auto& normals = *normalColumn;
                        for (int c = 0; c < 3; ++c) {
                            const std::size_t src = corner[c];
                            if (src >= normal->count()) {
                                continue;
                            }
                            normals[loops[static_cast<std::size_t>(c)].value] = {
                                normal->number(src, 0), normal->number(src, 1),
                                normal->number(src, 2)};
                        }
                    }
                }
            }
        }
    }

    if (out.mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, "no usable triangles in '" + path.string() + "'"};
    }
    if (skipped > 0) {
        out.droppedFaces = skipped;
        out.warnings.push_back("skipped " + std::to_string(skipped) + " degenerate triangle(s)");
    }
    computeBounds(out);
    return out;
}

Status exportGltf(const Mesh& mesh, const std::filesystem::path& path,
                  const ExportOptions& options) {
    // glTF supports triangles only: triangulate a copy, never the input.
    Mesh tri = mesh;
    tri.triangulate();

    const auto* colors = tri.vertexAttributes().find<Vec3>(kColorAttribute);
    const auto* uvs = options.writeUvs ? tri.cornerAttributes().find<Vec2>(kUvAttribute) : nullptr;
    const auto* normals =
        options.writeNormals ? tri.cornerAttributes().find<Vec3>(kNormalAttribute) : nullptr;

    // glTF attributes are per-vertex: split vertices by their distinct
    // (vertex, uv, normal) corner tuples so corner data survives exactly.
    using SplitKey = std::tuple<Index, std::uint64_t, std::uint64_t, std::uint32_t>;
    std::map<SplitKey, std::uint32_t> splitMap;
    std::vector<float> outPositions, outUvs, outNormals, outColors;
    std::vector<std::uint32_t> outIndices;

    auto keyOf = [&](LoopId l) -> SplitKey {
        const Index v = tri.loopVertex(l).value;
        std::uint64_t uvBits = 0;
        std::uint64_t nBitsA = 0;
        std::uint32_t nBitsB = 0;
        if (uvs) {
            const Vec2 t = (*uvs)[l.value];
            uvBits = (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(t.x)) << 32) |
                     std::bit_cast<std::uint32_t>(t.y);
        }
        if (normals) {
            const Vec3 n = (*normals)[l.value];
            nBitsA = (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(n.x)) << 32) |
                     std::bit_cast<std::uint32_t>(n.y);
            nBitsB = std::bit_cast<std::uint32_t>(n.z);
        }
        return {v, uvBits, nBitsA, nBitsB};
    };

    for (Index fi = 0; fi < tri.faceCapacity(); ++fi) {
        if (!tri.isAlive(FaceId{fi})) {
            continue;
        }
        for (const LoopId l : tri.faceLoops(FaceId{fi})) {
            const SplitKey key = keyOf(l);
            auto it = splitMap.find(key);
            if (it == splitMap.end()) {
                const auto newIndex = static_cast<std::uint32_t>(outPositions.size() / 3);
                it = splitMap.emplace(key, newIndex).first;
                const Vec3 p = tri.position(tri.loopVertex(l));
                outPositions.insert(outPositions.end(), {p.x, p.y, p.z});
                if (uvs) {
                    const Vec2 t = (*uvs)[l.value];
                    outUvs.insert(outUvs.end(), {t.x, t.y});
                }
                if (normals) {
                    const Vec3 n = (*normals)[l.value];
                    outNormals.insert(outNormals.end(), {n.x, n.y, n.z});
                }
                if (colors) {
                    const Vec3 c = (*colors)[tri.loopVertex(l).value];
                    outColors.insert(outColors.end(), {c.x, c.y, c.z});
                }
            }
            outIndices.push_back(it->second);
        }
    }

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "CyberRemesher";
    tinygltf::Buffer buffer;

    auto appendView = [&model, &buffer](const void* data, std::size_t bytes, int target) -> int {
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = buffer.data.size();
        view.byteLength = bytes;
        view.target = target;
        const auto* begin = static_cast<const unsigned char*>(data);
        buffer.data.insert(buffer.data.end(), begin, begin + bytes);
        while (buffer.data.size() % 4 != 0) {
            buffer.data.push_back(0);
        }
        model.bufferViews.push_back(view);
        return static_cast<int>(model.bufferViews.size() - 1);
    };
    auto appendAccessor = [&model](int view, int componentType, int type, std::size_t count) {
        tinygltf::Accessor accessor;
        accessor.bufferView = view;
        accessor.componentType = componentType;
        accessor.type = type;
        accessor.count = count;
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size() - 1);
    };

    tinygltf::Primitive prim;
    prim.mode = TINYGLTF_MODE_TRIANGLES;

    const std::size_t vertexCount = outPositions.size() / 3;
    {
        const int view =
            appendView(outPositions.data(), outPositions.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER);
        const int accessor =
            appendAccessor(view, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vertexCount);
        // POSITION accessors require min/max (glTF 2.0 spec).
        auto& a = model.accessors[static_cast<std::size_t>(accessor)];
        a.minValues = {1e30, 1e30, 1e30};
        a.maxValues = {-1e30, -1e30, -1e30};
        for (std::size_t i = 0; i < outPositions.size(); i += 3) {
            for (int c = 0; c < 3; ++c) {
                const double v = outPositions[i + static_cast<std::size_t>(c)];
                a.minValues[static_cast<std::size_t>(c)] =
                    std::min(a.minValues[static_cast<std::size_t>(c)], v);
                a.maxValues[static_cast<std::size_t>(c)] =
                    std::max(a.maxValues[static_cast<std::size_t>(c)], v);
            }
        }
        prim.attributes["POSITION"] = accessor;
    }
    if (!outUvs.empty()) {
        const int view = appendView(outUvs.data(), outUvs.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER);
        prim.attributes["TEXCOORD_0"] =
            appendAccessor(view, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, vertexCount);
    }
    if (!outNormals.empty()) {
        const int view =
            appendView(outNormals.data(), outNormals.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER);
        prim.attributes["NORMAL"] =
            appendAccessor(view, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vertexCount);
    }
    if (!outColors.empty()) {
        const int view =
            appendView(outColors.data(), outColors.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER);
        prim.attributes["COLOR_0"] =
            appendAccessor(view, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vertexCount);
    }
    {
        const int view = appendView(outIndices.data(), outIndices.size() * 4,
                                    TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        prim.indices = appendAccessor(view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                      TINYGLTF_TYPE_SCALAR, outIndices.size());
    }

    model.buffers.push_back(std::move(buffer));
    tinygltf::Mesh outMesh;
    outMesh.primitives.push_back(prim);
    model.meshes.push_back(outMesh);
    tinygltf::Node node;
    node.mesh = 0;
    model.nodes.push_back(node);
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    tinygltf::TinyGLTF writer;
    const bool binary = lowercaseExtension(path) == ".glb";
    if (!writer.WriteGltfSceneToFile(&model, path.string(), true, true, !binary, binary)) {
        return Error{ErrorCode::WriteFailed, "write to '" + path.string() + "' failed"};
    }
    return {};
}

}  // namespace cyber::io::detail
