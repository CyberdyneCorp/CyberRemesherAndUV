#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

#include "io_internal.hpp"

namespace cyber::io::detail {

namespace {

// STL has no shared-vertex connectivity; identical positions are welded so
// the imported mesh has real adjacency. Exact float bits as key keeps the
// weld deterministic.
struct Welder {
    Mesh& mesh;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, VertexId> seen;

    VertexId add(Vec3 p) {
        const auto key =
            std::tuple{std::bit_cast<std::uint32_t>(p.x), std::bit_cast<std::uint32_t>(p.y),
                       std::bit_cast<std::uint32_t>(p.z)};
        auto it = seen.find(key);
        if (it != seen.end()) {
            return it->second;
        }
        const VertexId v = mesh.addVertex(p);
        seen.emplace(key, v);
        return v;
    }
};

Result<ImportedMesh> importAsciiStl(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return Error{ErrorCode::ParseError, "cannot read '" + path.string() + "'"};
    }
    ImportedMesh out;
    Welder welder{out.mesh, {}};
    std::size_t skipped = 0;

    std::string token;
    std::vector<Vec3> corners;
    while (file >> token) {
        if (token == "vertex") {
            Vec3 p;
            if (!(file >> p.x >> p.y >> p.z)) {
                return Error{ErrorCode::ParseError, "malformed vertex in '" + path.string() + "'"};
            }
            corners.push_back(p);
        } else if (token == "endfacet") {
            if (corners.size() == 3) {
                const VertexId a = welder.add(corners[0]);
                const VertexId b = welder.add(corners[1]);
                const VertexId c = welder.add(corners[2]);
                if (!out.mesh.addFace(std::array{a, b, c}).valid()) {
                    ++skipped;
                }
            } else {
                ++skipped;
            }
            corners.clear();
        }
    }
    if (out.mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, "no usable facets in '" + path.string() + "'"};
    }
    if (skipped > 0) {
        out.warnings.push_back("skipped " + std::to_string(skipped) + " degenerate facet(s)");
    }
    computeBounds(out);
    return out;
}

Result<ImportedMesh> importBinaryStl(const std::filesystem::path& path, std::uintmax_t fileSize) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error{ErrorCode::ParseError, "cannot read '" + path.string() + "'"};
    }
    file.seekg(80);
    std::uint32_t count = 0;
    file.read(reinterpret_cast<char*>(&count), 4);
    if (!file || fileSize != 84 + static_cast<std::uintmax_t>(count) * 50) {
        return Error{ErrorCode::ParseError, "binary STL size mismatch in '" + path.string() + "'"};
    }

    ImportedMesh out;
    Welder welder{out.mesh, {}};
    std::size_t skipped = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        float data[12];  // normal + 3 corners
        file.read(reinterpret_cast<char*>(data), 48);
        file.seekg(2, std::ios::cur);  // attribute byte count
        if (!file) {
            return Error{ErrorCode::ParseError, "truncated binary STL '" + path.string() + "'"};
        }
        const VertexId a = welder.add({data[3], data[4], data[5]});
        const VertexId b = welder.add({data[6], data[7], data[8]});
        const VertexId c = welder.add({data[9], data[10], data[11]});
        if (!out.mesh.addFace(std::array{a, b, c}).valid()) {
            ++skipped;
        }
    }
    if (out.mesh.faceCount() == 0) {
        return Error{ErrorCode::EmptyMesh, "no usable facets in '" + path.string() + "'"};
    }
    if (skipped > 0) {
        out.warnings.push_back("skipped " + std::to_string(skipped) + " degenerate facet(s)");
    }
    computeBounds(out);
    return out;
}

}  // namespace

Result<ImportedMesh> importStl(const std::filesystem::path& path,
                               const ImportOptions& /*options*/) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size < 15) {
        return Error{ErrorCode::ParseError, "not an STL file: '" + path.string() + "'"};
    }
    // ASCII files start with "solid", but so can binary headers; trust the
    // size equation for the binary layout first.
    {
        std::ifstream probe(path, std::ios::binary);
        char header[6] = {};
        probe.read(header, 5);
        std::uint32_t count = 0;
        probe.seekg(80);
        probe.read(reinterpret_cast<char*>(&count), 4);
        const bool binarySize = size == 84 + static_cast<std::uintmax_t>(count) * 50;
        if (binarySize) {
            return importBinaryStl(path, size);
        }
        if (std::strncmp(header, "solid", 5) == 0) {
            return importAsciiStl(path);
        }
    }
    return Error{ErrorCode::ParseError, "unrecognized STL layout in '" + path.string() + "'"};
}

Status exportStl(const Mesh& mesh, const std::filesystem::path& path,
                 const ExportOptions& /*options*/) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return Error{ErrorCode::WriteFailed, "cannot open '" + path.string() + "' for writing"};
    }
    char header[80] = "CyberRemesher binary STL";
    file.write(header, 80);

    // Count triangles (n-gons fan-triangulated on the fly, mesh untouched).
    std::uint32_t count = 0;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (mesh.isAlive(FaceId{fi})) {
            count += static_cast<std::uint32_t>(mesh.faceSize(FaceId{fi}) - 2);
        }
    }
    file.write(reinterpret_cast<const char*>(&count), 4);

    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (!mesh.isAlive(FaceId{fi})) {
            continue;
        }
        const auto verts = mesh.faceVertices(FaceId{fi});
        const Vec3 n = mesh.faceNormal(FaceId{fi});
        for (std::size_t i = 2; i < verts.size(); ++i) {
            float data[12] = {n.x, n.y, n.z};
            const Vec3 corners[3] = {mesh.position(verts[0]), mesh.position(verts[i - 1]),
                                     mesh.position(verts[i])};
            for (int c = 0; c < 3; ++c) {
                data[3 + 3 * c] = corners[c].x;
                data[4 + 3 * c] = corners[c].y;
                data[5 + 3 * c] = corners[c].z;
            }
            file.write(reinterpret_cast<const char*>(data), 48);
            const std::uint16_t attr = 0;
            file.write(reinterpret_cast<const char*>(&attr), 2);
        }
    }
    file.flush();
    if (!file) {
        return Error{ErrorCode::WriteFailed, "write to '" + path.string() + "' failed"};
    }
    return {};
}

}  // namespace cyber::io::detail
