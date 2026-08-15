// libFuzzer entry point. Compiled once per target with
// -DCYBER_FUZZ_TARGET=<qualified function>, so the harnesses themselves stay
// free of libFuzzer-specific plumbing and the same code runs under the
// no-libFuzzer replay driver. See tests/fuzz/README.md.

#include "harness.hpp"

#ifndef CYBER_FUZZ_TARGET
#error "define CYBER_FUZZ_TARGET to one of cyber::fuzz::png / cyber::fuzz::meshIo"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return CYBER_FUZZ_TARGET(data, size);
}
