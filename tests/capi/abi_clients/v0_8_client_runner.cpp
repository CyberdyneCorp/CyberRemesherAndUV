#include <cstdio>
#include <cstring>

extern "C" int cyber_v0_8_client_smoke();

int main(int argc, char** argv) {
    std::fputs("legacy-client: entered main\n", stderr);
    std::fflush(stderr);
    if (argc == 2 && std::strcmp(argv[1], "--load-only") == 0) {
        std::fputs("legacy-client: shared library loaded\n", stderr);
        return 0;
    }
    std::fputs("legacy-client: calling cyber_version\n", stderr);
    std::fflush(stderr);
    const int status = cyber_v0_8_client_smoke();
    std::fprintf(stderr, "legacy-client: cyber_version returned %d\n", status);
    return status;
}
