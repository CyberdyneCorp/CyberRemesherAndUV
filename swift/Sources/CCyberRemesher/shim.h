/*
 * Umbrella shim for the CyberRemesher C ABI facade.
 *
 * This header exists only to give SwiftPM a stable, single include point for
 * the versioned C ABI. `cyber_capi.h` is NOT vendored here — it lives at
 * capi/include/cyber_capi.h and is located via the header search path
 * configured in Package.swift (or HEADER_SEARCH_PATHS in an Xcode consumer).
 *
 * capi/include/cyber_capi.h IS THE CONTRACT. The Swift layer binds it symbol
 * for symbol; there is no separate "assumed ABI" document to keep in sync (the
 * one that used to live in swift/README.md drifted from the header for its
 * whole life, because nothing built this package). Two checks hold the line:
 * the `swift-package` job in .github/workflows/ci.yml compiles it on macOS, and
 * tests/packaging/test_swift_abi_parity.py (ctest: `swift_abi_parity`) proves
 * on every platform that the Swift sources reference only symbols this header
 * declares, with the declared arities.
 */
#ifndef CYBER_REMESHER_SHIM_H
#define CYBER_REMESHER_SHIM_H

#include <cyber_capi.h>

#endif /* CYBER_REMESHER_SHIM_H */
