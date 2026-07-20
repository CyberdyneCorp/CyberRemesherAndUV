// Qt-free headless harness around AutoRemesher's QuadCover core (isotropic remesh ->
// seamless-UV parameterize -> isoline quad extract). Mirrors the QuadriFlow CLI so the
// benchmark can drive it: `autoremesher_cli -i in.obj -o out.obj -f <target_quads>`.
// AutoRemesher is MIT (huxingyi/autoremesher); this harness only calls its public API.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <geogram/basic/common.h>

#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Vector3>

namespace {

bool loadObj(const char* path, std::vector<AutoRemesher::Vector3>& verts,
             std::vector<std::vector<size_t>>& tris) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    char line[8192];
    std::vector<size_t> poly;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == 'v' && line[1] == ' ') {
            double x = 0, y = 0, z = 0;
            if (std::sscanf(line + 2, "%lf %lf %lf", &x, &y, &z) == 3) {
                verts.emplace_back(x, y, z);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            poly.clear();
            const char* p = line + 1;
            while (*p != '\0') {
                while (*p == ' ' || *p == '\t') {
                    ++p;
                }
                if (*p == '\0' || *p == '\n' || *p == '\r') {
                    break;
                }
                long idx = 0;
                if (std::sscanf(p, "%ld", &idx) == 1) {
                    const size_t vi = idx > 0 ? static_cast<size_t>(idx - 1)
                                              : static_cast<size_t>(
                                                    static_cast<long>(verts.size()) + idx);
                    poly.push_back(vi);
                }
                while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                    ++p;
                }
            }
            for (size_t i = 2; i < poly.size(); ++i) {  // fan-triangulate
                tris.push_back({poly[0], poly[i - 1], poly[i]});
            }
        }
    }
    std::fclose(f);
    return !verts.empty() && !tris.empty();
}

bool writeObj(const char* path, const std::vector<AutoRemesher::Vector3>& verts,
              const std::vector<std::vector<size_t>>& faces) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    for (const auto& v : verts) {
        std::fprintf(f, "v %.9g %.9g %.9g\n", v.x(), v.y(), v.z());
    }
    for (const auto& face : faces) {
        std::fprintf(f, "f");
        for (const size_t idx : face) {
            std::fprintf(f, " %zu", idx + 1);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* in = nullptr;
    const char* out = nullptr;
    long targetQuads = 3000;
    double scaling = 1.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            in = argv[++i];
        } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            targetQuads = std::atol(argv[++i]);
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            scaling = std::atof(argv[++i]);
        }
    }
    if (in == nullptr || out == nullptr) {
        std::fprintf(stderr, "usage: %s -i in.obj -o out.obj -f <target_quads>\n", argv[0]);
        return 2;
    }
    GEO::initialize();  // required before any Geogram use (parameterizer/quad_cover)
    std::vector<AutoRemesher::Vector3> verts;
    std::vector<std::vector<size_t>> tris;
    if (!loadObj(in, verts, tris)) {
        std::fprintf(stderr, "autoremesher_cli: failed to load %s\n", in);
        return 3;
    }
    AutoRemesher::AutoRemesher remesher(verts, tris);
    remesher.setTargetTriangleCount(static_cast<size_t>(targetQuads) * 2);  // GUI mapping
    remesher.setScaling(scaling);
    remesher.setModelType(AutoRemesher::ModelType::Organic);
    if (!remesher.remesh()) {
        std::fprintf(stderr, "autoremesher_cli: remesh failed\n");
        return 4;
    }
    std::fprintf(stderr, "[cli] input tris=%zu isotropic tris=%zu -> quads=%zu\n", tris.size(),
                 remesher.isotropicTriangles().size(), remesher.remeshedQuads().size());
    if (!writeObj(out, remesher.remeshedVertices(), remesher.remeshedQuads())) {
        std::fprintf(stderr, "autoremesher_cli: failed to write %s\n", out);
        return 5;
    }
    return 0;
}
