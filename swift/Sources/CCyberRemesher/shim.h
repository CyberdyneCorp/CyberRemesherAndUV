/*
 * UNVERIFIED: umbrella shim for the CyberRemesher C ABI facade.
 *
 * This header exists only to give SwiftPM a stable, single include point for
 * the versioned C ABI produced by the capi/ module. `cyber_capi.h` is NOT
 * vendored here — it is generated/installed by the capi/ build and located via
 * the header search path configured in Package.swift (or HEADER_SEARCH_PATHS in
 * an Xcode consumer). Nothing in this SwiftPM package can compile without it,
 * which is why the whole package is best-effort and disabled in headless CI.
 *
 * The Swift layer is written against the ABI contract documented in
 * swift/README.md ("Assumed C ABI contract"). If the capi module lands with
 * different spellings, update that contract and the thin call sites in
 * Sources/CyberRemesher/*.swift — the Swift-side design does not change.
 */
#ifndef CYBER_REMESHER_SHIM_H
#define CYBER_REMESHER_SHIM_H

#include <cyber_capi.h>

#endif /* CYBER_REMESHER_SHIM_H */
