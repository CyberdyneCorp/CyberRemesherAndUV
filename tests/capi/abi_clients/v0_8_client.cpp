#include <cstdint>

#include "v0_8_legacy.h"

extern "C" int cyber_v0_8_client_smoke() {
    int major = -1;
    int minor = -1;
    int patch = -1;
    cyber_version(&major, &minor, &patch);
    if (major < 0 || minor < 0 || patch < 0) {
        return 1;
    }
    return 0;
}
