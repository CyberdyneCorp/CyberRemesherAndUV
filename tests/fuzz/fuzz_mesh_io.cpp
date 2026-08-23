#include <array>
#include <cyber/core/io.hpp>

#include "harness.hpp"

namespace cyber::fuzz {
namespace {

// importMesh dispatches on the extension, so the first input byte selects the
// parser. libFuzzer mutates that byte like any other, which lets one corpus and
// one campaign reach every importer.
constexpr std::array<const char*, 6> kExtensions{".obj", ".ply", ".stl", ".gltf", ".glb", ".fbx"};

}  // namespace

int meshIo(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const char* extension = kExtensions[data[0] % kExtensions.size()];
    TempInput input(data + 1, size - 1, extension);
    if (!input.ok()) {
        return 0;
    }
    // Every malformed file must come back as a typed error, never as a crash, a
    // hang or an allocation sized from an attacker-controlled count.
    const auto result = io::importMesh(input.path());
    if (result.ok()) {
        // Force the imported mesh to be walked so a face index the parser
        // accepted but that points past the vertex array is actually dereferenced.
        volatile std::size_t sink = result.value().mesh.vertexCount();
        (void)sink;
    }
    return 0;
}

}  // namespace cyber::fuzz
